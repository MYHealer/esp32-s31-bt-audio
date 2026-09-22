/*
 * MIHC (Xiaomi High-quality Codec) decoder.
 *
 * MIHC's capability/negotiation layer lives in a2dp_vendor_mihc.c. This file
 * provides only the audio path, and it is built on the LHDC V5 decoder engine
 * (external/lhdc-v5/) rather than a separate algorithm library.
 *
 * WARNING - the reuse is KNOWN TO BE INCORRECT. Negotiation itself works
 * (2026-09-22: the phone selects MIHC and streams real frames - see
 * A2DP_VendorBuildCodecConfigMihc, which now allows negotiation so that
 * MIHC_FRAMEDUMP can capture over-the-air payloads), but playback is still
 * noise because the BES firmware's own MIHC decoder (MIHC1 1.2.1 / MIHC2
 * 2.1.3, core symbol mlac_dec_init) differs from LHDC V5 in ways the LHDC
 * engine cannot absorb:
 *
 *   - MIHC frames carry a sync word and a container header, parsed by
 *     parse_header() / mihc_check_header_info(); LHDC V5 has neither and slices
 *     frames by a fixed length (see lhdc_dec.c).
 *   - MIHC has a CRC layer (MILL__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH)
 *     and metadata; LHDC V5 has neither.
 *   - MIHC's decoder init takes a run-time `blocksize`; LHDC derives the frame
 *     sample count from the compile-time A2DP_LHDCV5_OUTPUT_FRAME_SAMPLES.
 *   - MIHC's channel mode is adaptive (ch_adpt_lvl / ch_type); LHDC's is fixed.
 *
 * A decoder built on the LHDC engine therefore emits noise, not PCM. This file
 * is kept because the SEP plumbing around it is correct and reusable - replace
 * the engine with a real MLAC implementation, then re-enable negotiation in
 * a2dp_vendor_mihc.c.
 *
 * This is deliberately a *clone* of a2dp_vendor_lhdcv5_decoder.c rather than a
 * shared implementation, so that a divergence discovered while bringing MIHC up
 * can be fixed here without risking LHDC regression.
 *
 * The LHDC engine is instantiated once per active codec, and only one A2DP
 * codec streams at a time, so MIHC and LHDC never hold workspace concurrently.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/bt_trace.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lhdc_dec.h"
#include "lhdc_entropy_dec.h"
#include "lhdc_imdct.h"
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
/* Hex-dump real over-the-air payloads. This is the primary bring-up tool for
 * MIHC: MIHC reuses the LHDC engine, so the first question when there is no
 * audio is whether the payload is framed/descrambled the same way. Dumps a
 * burst of packets once per stream, tagged MIHC_FDUMP for easy grep. */
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
 * The decoder workspace (~39 KB at 192k) is allocated from the MAIN internal
 * heap via heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT), NOT osi_malloc.
 * The bluedroid/osi heap fragments down to a ~15 KB largest free block during a
 * call, so osi_malloc(39 KB) fails there and the decoder would silently drop
 * every packet. Allocated ONCE at init, reused across reconfigurations, freed
 * on cleanup.
 */
#define MIHC_WS_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
typedef struct {
    void *workspace;             /* heap block holding the decoder + rate-sized tail */
    size_t workspace_size;       /* bytes currently allocated (rate-sized) */
    lhdc_decoder_t *decoder;
    int fragment_size;
    int fragment_count;
    uint8_t fragment[MIHC_FRAGMENT_BUF_SIZE];
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
    void  *ws  = cb->workspace;      /* keep an already-allocated block across re-init */
    size_t wss = cb->workspace_size;
    memset(cb, 0, sizeof(*cb));
    cb->decode_callback = decode_callback;
    /* Workspace is RATE-SIZED, so it is (re)allocated in decoder_configure once
     * the negotiated sample rate is known. Keep any existing block here. */
    cb->workspace = ws;
    cb->workspace_size = wss;
    return true;
}

void a2dp_mihc_decoder_cleanup(void) {
    /* Release the lazily-allocated IMDCT tables and entropy FAC models since
     * MIHC is going away — so none of the decoder's scratch lingers in DRAM
     * while LDAC/SBC is the active codec. */
    lhdc_imdct_free_960();
    lhdc_imdct_free_480();
    lhdc_imdct_free_1920();
    lhdc_imdct_free_cos();
    lhdc_dec_free_window();
    lhdc_entropy_free();
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    if (!cb) return;
    if (cb->workspace) {
        heap_caps_free(cb->workspace);
    }
    free(cb);
    s_mihc_cb = NULL;
}

void a2dp_mihc_decoder_configure(const uint8_t* p_codec_info) {
    tA2DP_MIHC_DECODER_CB *cb = s_mihc_cb;
    if (!cb) return;
    tA2DP_MIHC_CIE cie;
    if (A2DP_ParseInfoMihc(&cie, p_codec_info, false) != A2D_SUCCESS) {
        MIHC_LOGE("failed to parse MIHC CIE");
        return;
    }

    cb->decoder = NULL;   /* re-init below */

    lhdc_dec_config_t config = {
        .sample_rate = LHDC_DEC_SR_48000,
        .bit_depth = LHDC_DEC_BITDEPTH_16,
        .frame_duration = LHDC_DEC_FRAME_5MS,   /* MIHC shares the 5 ms frame cadence */
        .channels = 2,
        .max_frame_bytes = MIHC_FRAGMENT_BUF_SIZE,
        .lossless_enable = 0,
    };

    /* MIHC advertises 48 / 44.1 / 32 kHz (see a2dp_mihc_sink_caps). The LHDC
     * engine picks its band config by samples-per-frame (sr*dur/1000), not by an
     * enumerated rate, so 32 kHz (160 spf) lands on the same 480-hop HR config
     * as 44.1/48 kHz — no extra table entry is needed. */
    if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_48000) {
        config.sample_rate = LHDC_DEC_SR_48000;
        a2dp_mihc_current_sample_rate = 48000;
    } else if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_44100) {
        config.sample_rate = LHDC_DEC_SR_44100;
        a2dp_mihc_current_sample_rate = 44100;
    } else if (cie.sampleRate & A2DP_MIHC_SAMPLING_FREQ_32000) {
        /* The LHDC engine has no 32 kHz enum; feed it 44100 so lhdc_get_band_cfg()
         * (which only looks at samples-per-frame) resolves correctly while the
         * reported rate stays 32000 and drives the downstream ASRC. 32000*5/1000 =
         * 160 spf, and 44100*5/1000 = 220 spf; both take the <=240 HR branch, so
         * the band config (and therefore the MDCT geometry) is identical. */
        config.sample_rate = LHDC_DEC_SR_44100;
        a2dp_mihc_current_sample_rate = 32000;
    } else {
        MIHC_LOGW("MIHC peer offers no rate we implement (cie.sampleRate=0x%02x), using 48k",
                  cie.sampleRate);
        config.sample_rate = LHDC_DEC_SR_48000;
        a2dp_mihc_current_sample_rate = 48000;
    }

    /* Force 16-bit output to match the I2S/PCM pipeline. */
    config.bit_depth = LHDC_DEC_BITDEPTH_16;
    a2dp_mihc_current_bit_depth = 16;
    config.channels = 2;

    /* GROW-ONLY workspace, MAX-SIZED for 192k on the FIRST config, then REUSED
     * for every rate with zero realloc. Grabbing the full block while the heap
     * is still pristine guarantees the rate switch cannot fail mid-stream (the
     * heap fragments during a call and a later grow can find no hole). MIHC
     * itself only negotiates up to 48 kHz, but the LHDC engine's per-rate
     * carve-out has no smaller "safety" size, so take the same max as LHDC. */
    {
        size_t need = lhdc_dec_get_workspace_size(LHDC_DEC_SR_192000, config.frame_duration);
        if (!cb->workspace || cb->workspace_size < need) {
            if (cb->workspace) heap_caps_free(cb->workspace);
            cb->workspace = heap_caps_malloc(need, MIHC_WS_CAPS);
            if (!cb->workspace) {
                cb->workspace_size = 0;
                MIHC_LOGE("configure: cannot allocate %u-byte workspace (largest free %u)",
                          (unsigned)need,
                          (unsigned)heap_caps_get_largest_free_block(MIHC_WS_CAPS));
                return;
            }
            cb->workspace_size = need;
            MIHC_LOGI("configure: workspace %u bytes (max-sized; rate=%u)",
                      (unsigned)need, (unsigned)a2dp_mihc_current_sample_rate);
        }
    }

    cb->decoder = lhdc_dec_init(cb->workspace, &config);
    cb->fragment_size = 0;
    cb->fragment_count = 0;
#if MIHC_FRAMEDUMP
    s_dump_sr = config.sample_rate; s_fd_count = 0;   /* fresh capture per stream */
#endif

    if (!cb->decoder) {
        MIHC_LOGE("lhdc_dec_init failed for MIHC");
        return;
    }

    MIHC_LOGI("decoder configured: sr=%d depth=%d ch=%d dur=%d",
              (int)a2dp_mihc_current_sample_rate, (int)config.bit_depth,
              (int)config.channels, (int)config.frame_duration);
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
    if (!p_buf || !buf || !cb->decoder || !cb->decode_callback) {
        static uint32_t s_e = 0;
        if ((s_e++ % 200) == 0) MIHC_LOGW("decode_packet null guard (dec=%p)", (void*)cb->decoder);
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
        if (src_size > (sizeof(cb->fragment) - (size_t)cb->fragment_size)) {
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
     * One A2DP payload is a concatenation of frames, each [u16 LE header]
     * [payload]. Decode them in a loop until the input is exhausted, emitting
     * each frame's PCM to the sink immediately (no large accumulation buffer —
     * DRAM on ESP32 is scarce).
     *
     * The leading byte before the frame stream is the LHDC seq/marker byte that
     * is not part of the raw encoder vectors (see a2dp_vendor_lhdcv5_decoder.c
     * for how it was identified: offset 1 as LE [u16 len] was the unique start
     * whose descrambled flag bit read 0). MIHC is handled the same way; if the
     * frame dump shows MIHC does not carry that byte, this offset is the first
     * thing to change.
     */
    size_t off = (src_size >= 1) ? 1 : 0;
    int frames = 0;
    size_t pcm_emitted = 0;
    uint8_t out_channels = 2, out_depth = 16;
    (void)out_channels; (void)out_depth;

    while (off + 2 <= src_size) {
        /*
         * MIHC adaptation layer (BES func_01DE1C / func_01DEE8).
         *
         * The LHDC engine parses the frame header as
         *   fhdr = in[0] | (in[1] << 8)   (LE)
         *   payload_len = (fhdr & 0x3FF) * 2
         *   flags       = fhdr >> 10
         * and then DISCARDS `flags` entirely -- LHDC carries its stream params
         * in the A2DP CIE, not in the frame. The BES MIHC adaptation layer, by
         * contrast, USES the high bits: its frame-type detector reads three
         * sub-fields (recovered as [sp+9] / [sp+0xB] / [sp+0xC]) and returns
         *   3 = normal frame, 9 = sub==1, 0xA = sub==1, 4 = sub==2,
         *   7 = unknown -> retry / resync.
         *
         * We cannot invalidate a frame on `flags` alone: the BES sub-field
         * mapping onto fhdr bits is not yet pinned down (it needs a real
         * captured stream to correlate against), and rejecting a frame the
         * engine would have decoded costs audio. So the flags are decoded and
         * REPORTED rather than enforced, which is exactly the evidence needed
         * to finish the mapping. Frame acceptance stays with the engine: a
         * frame it cannot parse yields LHDC_DEC_BITSTREAM_ERROR below and ends
         * the loop, which is the same outcome a stricter check would produce.
         */
        uint16_t fhdr = (uint16_t)(src[off] | (src[off + 1] << 8));
        uint16_t flags = (uint16_t)(fhdr >> 10);
        uint16_t plen  = (uint16_t)((fhdr & 0x03FF) * 2);

#if MIHC_FRAMEDUMP
        /* Per-frame header trace, bounded to the same burst as the payload
         * dump so a captured log stays small. Correlating these flags against
         * the dumped payload is what pins the frame-type mapping. */
        {
            static uint32_t s_hdr_lines = 0;
            if (s_hdr_lines < 40) {
                s_hdr_lines++;
                MIHC_LOGI("MIHC_HDR off=%u fhdr=0x%04x len=%u flags=0x%02x "
                          "(f2=%u f3=%u f5=%u)",
                          (unsigned)off, fhdr, plen, flags,
                          (unsigned)((flags >> 0) & 1),
                          (unsigned)((flags >> 1) & 1),
                          (unsigned)((flags >> 3) & 1));
            }
        }
#endif
        if (plen == 0 || off + 2 + plen > src_size) {
            /* Truncated / not a frame. Report it and let the engine make the
             * call: it returns LHDC_DEC_NEED_MORE_DATA (frame spans packets) or
             * LHDC_DEC_BITSTREAM_ERROR, and the loop ends on either. Keeping the
             * decision in the engine preserves the stock LHDC control flow --
             * this check only annotates what the header claimed. */
            static uint32_t s_trunc = 0;
            if ((s_trunc++ % 20) == 0) {
                MIHC_LOGW("frame header out of range: off=%u len=%u avail=%u",
                          (unsigned)off, plen, (unsigned)(src_size - off));
            }
        }

        size_t consumed = 0;
        uint32_t generated = 0;
        lhdc_dec_frame_info_t info;
        lhdc_dec_ret_t ret = lhdc_dec_decode_frame(cb->decoder, src + off,
                                                   src_size - off, buf, 1920,
                                                   &consumed, &generated, &info);
        if (ret != LHDC_DEC_OK || consumed == 0) {
            static uint32_t s_errs = 0;
            if (frames == 0 && (s_errs++ % 20) == 0) {
                MIHC_LOGW("decode ret=%d (%s) off=%u/%u (err#%u) hdr=%02x%02x "
                          "flags=0x%02x",
                          ret, lhdc_dec_strerror(ret), (unsigned)off,
                          (unsigned)src_size, s_errs,
                          src_size >= 2 ? src[off] : 0,
                          src_size >= 2 ? src[off + 1] : 0, flags);
            }
            break;
        }
        out_channels = info.channels;
        out_depth = info.bit_depth;
        size_t pcm_bytes = (size_t)generated * info.channels * (info.bit_depth / 8);
        cb->decode_callback((uint8_t*)buf, pcm_bytes);
        pcm_emitted += pcm_bytes;
        off += consumed;
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
            MIHC_LOGI("RX pkt#%u frames=%d pcm=%u (ch=%d depth=%d) totals: %u frames, %u KiB",
                      s_packets, frames, (unsigned)pcm_emitted,
                      out_channels, out_depth, s_frames, s_pcm / 1024);
        }
    }
#endif
    return true;
}

#endif /* defined(MIHC_DEC_INCLUDED) && MIHC_DEC_INCLUDED == TRUE) */
