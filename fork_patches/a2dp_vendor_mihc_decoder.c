/*
 * MIHC (Xiaomi High-quality Codec) decoder.
 *
 * MIHC's capability/negotiation layer lives in a2dp_vendor_mihc.c. This file
 * provides the A2DP audio path for the BT carrier.
 *
 * BT carrier is Opus-shaped: each 626-byte frame is
 *   [seq:1][71 02:2][ff fb 5a 20:4][crc8:1][L:BE16=614][TOC+EC:614][crc16:2]
 * The payload starts at TOC 0xF4 and is decoded by mihc_celt_decode_with_ec()
 * (modified libopus CELT — NOT MLAC/FLAC, which is the ENC-file carrier).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/bt_trace.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mihc_celt/mihc_pvq.h"
#include "osi/allocator.h"
#include "stack/a2dp_vendor_mihc.h"
#include "stack/a2dp_vendor_mihc_decoder.h"

#if (defined(MIHC_DEC_INCLUDED) && MIHC_DEC_INCLUDED == TRUE)

#define MIHC_TAG "MIHC_DEC"
#define MIHC_LOGI(...) ESP_LOGI(MIHC_TAG, __VA_ARGS__)
#define MIHC_LOGW(...) ESP_LOGW(MIHC_TAG, __VA_ARGS__)
#define MIHC_LOGE(...) ESP_LOGE(MIHC_TAG, __VA_ARGS__)

/* Per-packet decode logging. OFF: each ESP_LOGI blocks the decode task ~9 ms on
 * the 115200-baud UART and audibly stutters playback. */
#define MIHC_VERBOSE 0
/* Hex-dump real over-the-air payloads. Primary bring-up tool: confirms the
 * 626-byte BT frame layout (seq|71 02|ff fb 5a 20|crc8|L|TOC+EC|crc16) before
 * trusting the CELT path. Dumps a burst once per stream, tagged MIHC_FDUMP. */
#define MIHC_FRAMEDUMP 1
static uint32_t s_dump_sr = 0;
static uint32_t s_fd_count = 0;   /* reset per stream in configure */

/* Same sizing rationale as the LHDC decoder: a 96k/5ms HR frame is <~1.3 KB,
 * so 3 KB covers any fragmented frame without wasting .bss. */
#define MIHC_FRAGMENT_BUF_SIZE (3 * 1024)
/* Decode output buffer. Must match BT_A2DP_SINK_BUF_MIHC in btc_a2dp_sink.c.
 * 4 KB so 24-bit content emitted in 32-bit containers fits up to 192k. */
#define MIHC_MAX_PCM_BYTES     (4 * 2000)

/*
 * MIHC CELT decoder keeps cross-frame state (OLA decode_mem / preemph /
 * postfilter) in file-scope tables inside mihc_celt_decode.c — about 17 KB
 * of .bss, no heap workspace. The CB only needs the fragment buffer and the
 * I2S callback. PSRAM is still preferred for the 3 KB fragment block.
 */
#if CONFIG_SPIRAM
#define MIHC_WS_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define MIHC_WS_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
typedef struct {
    uint8_t *workspace;              /* fragment reassembly block (3 KB) */
    size_t   workspace_size;
    int      fragment_size;
    int      fragment_count;
    uint8_t *fragment;               /* points into workspace */
    decoded_data_callback_t decode_callback;
} tA2DP_MIHC_DECODER_CB;

/* Allocated lazily on init, freed on cleanup. The control block embeds a 3 KB
 * fragment-reassembly buffer; keeping it off .bss frees that RAM whenever MIHC
 * is not the active codec. */
static tA2DP_MIHC_DECODER_CB *s_mihc_cb = NULL;

/* Exposed to app layer for ASRC/bit-depth reconfiguration on rate change */
uint32_t a2dp_mihc_current_sample_rate = 0;
uint8_t  a2dp_mihc_current_bit_depth = 16;

bool a2dp_mihc_decoder_init(decoded_data_callback_t decode_callback) {
    if (!s_mihc_cb) {
        s_mihc_cb = (tA2DP_MIHC_DECODER_CB *)calloc(1, sizeof(*s_mihc_cb));
        if (!s_mihc_cb) {
            MIHC_LOGE("cannot allocate MIHC decoder cb");
            return false;
        }
    }
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    memset(cb, 0, sizeof(*cb));
    cb->decode_callback = decode_callback;
    MIHC_LOGI("init ENTER cb=%p cbk=%p", (void*)cb, (void*)decode_callback);
    return true;
}

void a2dp_mihc_decoder_cleanup(void) {
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    if (!cb) return;
    if (cb->workspace) {
        heap_caps_free(cb->workspace);
        cb->workspace = NULL;
    }
    free(cb);
    s_mihc_cb = NULL;
}

void a2dp_mihc_decoder_configure(const uint8_t* p_codec_info) {
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    MIHC_LOGI("configure ENTER cb=%p info=%p", (void*)cb, (const void*)p_codec_info);
    if (!cb) {
        MIHC_LOGE("configure: no cb (init not called)");
        return;
    }
    tA2DP_MIHC_CIE cie;
    if (A2DP_ParseInfoMihc(&cie, p_codec_info, false) != A2D_SUCCESS) {
        MIHC_LOGE("failed to parse MIHC CIE");
        return;
    }
    MIHC_LOGI("configure: CIE parsed sr_mask=0x%02x chMode=%d bps=%d ver=0x%02x",
              cie.sampleRate, cie.channelMode, cie.bitsPerSample, cie.version);

    /* MIHC BT carrier is 48 kHz / 20 ms / stereo (TOC 0xF4 → LM=2 → N=480).
     * CIE rate bits are recorded for the app layer; the CELT mode table is 48k. */
    if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_48000) {
        a2dp_mihc_current_sample_rate = 48000;
    } else if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_44100) {
        a2dp_mihc_current_sample_rate = 44100;
    } else if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_32000) {
        a2dp_mihc_current_sample_rate = 32000;
    } else {
        MIHC_LOGW("MIHC peer offers no rate we implement (cie.sampleRate=0x%02x), using 48k",
                  cie.sampleRate);
        a2dp_mihc_current_sample_rate = 48000;
    }

    /* Force 16-bit output to match the I2S/PCM pipeline. */
    a2dp_mihc_current_bit_depth = 16;

    /* Fragment reassembly block (fixed 3 KB, matches MIHC_FRAGMENT_BUF_SIZE). */
    if (!cb->workspace) {
        cb->workspace = heap_caps_malloc(MIHC_FRAGMENT_BUF_SIZE, MIHC_WS_CAPS);
        if (!cb->workspace) {
            MIHC_LOGE("configure: cannot allocate %u-byte fragment buf (largest free %u)",
                      (unsigned)MIHC_FRAGMENT_BUF_SIZE,
                      (unsigned)heap_caps_get_largest_free_block(MIHC_WS_CAPS));
            return;
        }
        cb->workspace_size = MIHC_FRAGMENT_BUF_SIZE;
    }
    cb->fragment = cb->workspace;

    /* Cross-frame CELT state (OLA / preemph / postfilter) lives in .bss. */
    mihc_celt_reset();
    cb->fragment_size = 0;
    cb->fragment_count = 0;
#if MIHC_FRAMEDUMP
    s_dump_sr = a2dp_mihc_current_sample_rate; s_fd_count = 0;  /* fresh capture per stream */
#endif

    MIHC_LOGI("decoder configured (MIHC CELT): sr=%d depth=%d ch=2",
              (int)a2dp_mihc_current_sample_rate, (int)a2dp_mihc_current_bit_depth);
}

ssize_t a2dp_mihc_decoder_decode_packet_header(BT_HDR* p_buf) {
    if (!p_buf) return -EINVAL;

    const size_t header_len = sizeof(struct media_packet_header) +
                              sizeof(struct media_payload_header);
    if (p_buf->len < header_len) {
        APPL_TRACE_ERROR("%s: packet too short", __func__);
        return -EINVAL;
    }

    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    if (!cb) return -EINVAL;
    uint8_t* src = ((uint8_t *)(p_buf + 1)) + p_buf->offset;
    struct media_payload_header* payload =
        (struct media_payload_header*)(src + sizeof(struct media_packet_header));

    if (payload->is_fragmented) {
        if (payload->is_first_fragment) {
            cb->fragment_size = 0;
        } else if (payload->frame_count + 1 != cb->fragment_count ||
                   (payload->frame_count == 1 && !payload->is_last_fragment)) {
            cb->fragment_count = 0;
            cb->fragment_size = 0;
            return -EINVAL;
        }
        cb->fragment_count = payload->frame_count;
    } else {
        /* Do NOT reject frame_count==0 here: for LHDC-family payloads the
         * leading byte is a seq/marker byte, not an SBC frame counter, so this
         * 4-bit field reads 0 when the phone packs several small frames per
         * packet. A payload with no decodable frame is dropped later by
         * decode_packet() (frames==0). */
        cb->fragment_count = 0;
        cb->fragment_size = 0;
    }

    p_buf->offset += header_len;
    p_buf->len -= header_len;
    return 0;
}

bool a2dp_mihc_decoder_decode_packet(BT_HDR* p_buf, unsigned char* buf,
                                     size_t buf_len) {
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    if (!cb) return false;
    if (!p_buf || !buf || !cb->decode_callback) {
        static uint32_t s_e = 0;
        if ((s_e++ % 200) == 0) MIHC_LOGW("decode_packet null guard");
        return false;
    }
    if (buf_len < MIHC_MAX_PCM_BYTES) {
        static uint32_t s_b = 0;
        if ((s_b++ % 200) == 0) MIHC_LOGW("decode buf too small: %u < %u",
                                          (unsigned)buf_len, (unsigned)MIHC_MAX_PCM_BYTES);
        return false;
    }

    uint8_t* src = ((uint8_t *)(p_buf + 1)) + p_buf->offset;
    size_t src_size = p_buf->len;

#if MIHC_FRAMEDUMP
    /* Capture REAL over-the-air payloads so the framing can be compared against
     * the LHDC layout this decoder expects. Dumps a consecutive burst once per
     * stream, past startup, tagged MIHC_FDUMP. */
    {
        uint32_t i = s_fd_count++;
        if (i >= 60 && i < 140) {
            char line[1600]; int p = 0;
            p += snprintf(line+p, sizeof(line)-p, "MIHC_FDUMP#%u len=%u sr=%u:",
                          i, (unsigned)src_size, (unsigned)s_dump_sr);
            for (size_t b = 0; b < src_size && p < (int)sizeof(line)-4; b++)
                p += snprintf(line+p, sizeof(line)-p, "%02x", src[b]);
            MIHC_LOGI("%s", line);
        }
    }
#endif

    if (cb->fragment_count > 0) {
        if (src_size > (MIHC_FRAGMENT_BUF_SIZE - (size_t)cb->fragment_size)) {
            cb->fragment_count = 0;
            cb->fragment_size = 0;
            APPL_TRACE_ERROR("%s: fragmented MIHC frame too large", __func__);
            return false;
        }

        memcpy(cb->fragment + cb->fragment_size, src, src_size);
        cb->fragment_size += (int)src_size;

        if (cb->fragment_count > 1) return true;

        src = cb->fragment;
        src_size = (size_t)cb->fragment_size;
        cb->fragment_count = 0;
        cb->fragment_size = 0;
    }

    /*
     * BT carrier frame (626 B), from real OTA captures:
     *   [seq:1][71 02:2][ff fb 5a 20:4][crc8:1][L:BE16=614][TOC+EC:614][crc16:2]
     * The CELT payload starts at TOC (0xF4) = sync+7 where sync is `ff fb 5a`.
     * One A2DP payload may pack several frames; step by L+9 from the sync.
     */
    size_t off = 0;
    int frames = 0;
    size_t pcm_emitted = 0;

    /* mihc_celt_decode_with_ec only NULL-checks `st`; cross-frame state is
     * file-scope in mihc_celt_decode.c. Keep a host-sized dummy block. */
    static uint8_t s_celt_st[4096];

    while (off + 10 <= src_size) {
        uint32_t sync_at = 0xFFFFFFFFu;
        for (size_t i = off; i + 2 < src_size; i++) {
            if (src[i] == 0xFF && src[i + 1] == 0xFB && src[i + 2] == 0x5A) {
                sync_at = (uint32_t)i;
                break;
            }
        }
        if (sync_at == 0xFFFFFFFFu) {
            static uint32_t s_nosync = 0;
            if ((s_nosync++ % 20) == 0)
                MIHC_LOGW("no frame sync in %u bytes at off=%u",
                          (unsigned)(src_size - off), (unsigned)off);
            break;
        }

        /* L = payload length at sync+5 (crc8 occupies sync+4). */
        if (sync_at + 7 > src_size) break;
        uint32_t plen = ((uint32_t)src[sync_at + 5] << 8) | src[sync_at + 6];
        if (plen < 2 || plen > 614) {
            static uint32_t s_badlen = 0;
            if ((s_badlen++ % 20) == 0)
                MIHC_LOGW("bad L=%u at sync@%u", (unsigned)plen, (unsigned)sync_at);
            off = sync_at + 3;
            continue;
        }
        const uint8_t *payload = src + sync_at + 7;   /* TOC 0xF4 */
        if (sync_at + 7 + plen > src_size) break;

        int16_t *pcm = (int16_t *)buf;
        int32_t rc = mihc_celt_decode_with_ec((mihc_celt_dec *)s_celt_st,
                                              payload, (int32_t)plen,
                                              pcm, 2, NULL, NULL, NULL);
        if (rc < 0) {
            static uint32_t s_errs = 0;
            if (frames == 0 && (s_errs++ % 20) == 0) {
                MIHC_LOGW("celt decode rc=%d sync@%u plen=%u (err#%u)",
                          (int)rc, (unsigned)sync_at, (unsigned)plen, s_errs);
            }
            off = sync_at + 3;
            continue;
        }
        /* TOC 0xF4 → LM=2 → N=480 samples, stereo int16 interleaved. */
        size_t pcm_bytes = 480 * 2 * 2;
        cb->decode_callback((uint8_t*)buf, pcm_bytes);
        pcm_emitted += pcm_bytes;
        off = sync_at + 7 + plen + 2;   /* skip payload + crc16 */
        frames++;
    }

    if (frames == 0) {
        return false;
    }

#if MIHC_VERBOSE
    {
        static uint32_t s_packets = 0, s_frames = 0, s_pcm = 0;
        s_packets++; s_frames += frames; s_pcm += pcm_emitted;
        if ((s_packets % 100) == 1) {
            MIHC_LOGI("RX pkt#%u frames=%d pcm=%u totals: %u frames, %u KiB",
                      s_packets, frames, (unsigned)pcm_emitted,
                      s_frames, s_pcm / 1024);
        }
    }
#endif
    return true;
}

#endif /* defined(MIHC_DEC_INCLUDED) && MIHC_DEC_INCLUDED == TRUE) */
