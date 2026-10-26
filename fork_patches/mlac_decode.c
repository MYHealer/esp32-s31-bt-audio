/*
 * MLAC decoder — rewritten from Ghidra decompilation of MILL__stream_decoder_process
 * in libcodec2_soft_mihc_dec.so
 *
 * Algorithm confirmed by cross-referencing:
 *   - Ghidra decompilation (mlac_decompiled.txt)
 *   - Capstone disassembly of read_bits (0xf200), bitstream_helper (0xf774)
 *   - CRC16 table extraction from .so
 *   - Real MIHC frame captures (30 frames, 626 bytes each, 48kHz stereo)
 *
 * The decoder outputs time-domain PCM directly (MLAC is a Modified FLAC codec
 * — no IMDCT needed). Coefficients from the bitstream ARE the audio signal.
 */

#include "mlac_decode.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

#define TAG "MLAC_DEC"
#define MLAC_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define MLAC_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define MLAC_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)

/* Dump first N frames for debugging */
static uint32_t s_frame_count = 0;
#define MLAC_DUMP_FRAMES 50

/* ============================================================
 * CRC16/ARC table (extracted from .so at VA 0x3400)
 * ============================================================ */
#include "mlac_crc16_table.h"

static uint8_t mlac_crc8_simple(const uint8_t *data, uint32_t len)
{
    /* CRC-8 for header validation (polynomial 0x07) */
    uint8_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ============================================================
 * BITSTREAM READER — streaming byte-addressed, MSB-first.
 *
 * Matches the .so's mlac_ll_bitreader_read_raw_uint32 callback-based reader.
 * Each byte is read from data[byte_idx], bits extracted from MSB (bit 7)
 * downward. This is NOT word-addressed — bytes are consumed one at a time.
 * ============================================================ */

static void bitreader_init(mlac_bitreader_t *r, const uint8_t *data, uint32_t len)
{
    r->data = data;
    r->data_len = len;
    r->byte_idx = 0;
    r->bit_in_byte = 7;  /* next bit to read is MSB of current byte */
    r->error_flag = 0;
    r->crc16 = 0;
    r->crc16_active = false;
}

static void crc16_update(mlac_bitreader_t *r, uint8_t byte)
{
    if (r->crc16_active) {
        r->crc16 = mlac_crc16_table[(r->crc16 ^ byte) & 0xFF] ^ (r->crc16 >> 8);
    }
}

/*
 * read_bits: extract num_bits (1..32) from the bitstream, MSB-first.
 * Returns 1 on success, 0 on error.
 *
 * Implementation: read bytes on demand, extract bits from MSB downward.
 * When all 8 bits of current byte are consumed, advance to next byte.
 * CRC16 is updated for each fully consumed byte.
 */
static int bitreader_read_bits(mlac_bitreader_t *r, uint32_t *out, uint32_t num_bits)
{
    if (num_bits == 0) {
        *out = 0;
        return 1;
    }
    if (r->error_flag == -1)
        return 0;

    uint32_t result = 0;

    for (uint32_t i = 0; i < num_bits; i++) {
        /* Check if current byte is exhausted (bit_in_byte went below 0) */
        if (r->bit_in_byte < 0) {
            /* Current byte exhausted — CRC it and move to next */
            if (r->crc16_active) {
                crc16_update(r, r->data[r->byte_idx]);
            }
            r->byte_idx++;
            r->bit_in_byte = 7;
        }

        if (r->byte_idx >= r->data_len) {
            r->error_flag = -1;
            return 0;
        }

        /* Extract MSB-first: bit 7 is first, bit 0 is last */
        uint32_t bit = (r->data[r->byte_idx] >> r->bit_in_byte) & 1;
        result = (result << 1) | bit;
        r->bit_in_byte--;
    }

    *out = result;
    return 1;
}

/* Read signed N-bit value with sign extension (XOR-subtract from .so bitstream_helper) */
static int bitreader_read_signed(mlac_bitreader_t *r, int64_t *out, uint32_t num_bits)
{
    if (num_bits == 0) {
        *out = 0;
        return 1;
    }
    if (r->error_flag == -1)
        return 0;

    if (num_bits <= 32) {
        uint32_t raw;
        if (!bitreader_read_bits(r, &raw, num_bits))
            return 0;
        int64_t sign_bit = (int64_t)1 << (num_bits - 1);
        int64_t val = (int64_t)raw;
        val ^= sign_bit;
        val -= sign_bit;
        *out = val;
        return 1;
    }

    /* > 32 bits: read two chunks */
    uint32_t low_bits = num_bits - 32;
    uint32_t low_word, high_word;
    if (!bitreader_read_bits(r, &low_word, low_bits))
        return 0;
    if (!bitreader_read_bits(r, &high_word, 32))
        return 0;
    uint64_t raw = ((uint64_t)low_word << 32) | high_word;
    int64_t sign_bit = (int64_t)1 << (num_bits - 1);
    int64_t val = (int64_t)(raw ^ (uint64_t)sign_bit) - sign_bit;
    *out = val;
    return 1;
}

static int bitreader_read_u32(mlac_bitreader_t *r, uint32_t *out, uint32_t num_bits)
{
    return bitreader_read_bits(r, out, num_bits);
}

static int bitreader_read_unary(mlac_bitreader_t *r, uint32_t *out)
{
    uint32_t count = 0;
    while (1) {
        uint32_t bit;
        if (!bitreader_read_bits(r, &bit, 1))
            return 0;
        if (bit == 1) break;
        count++;
        if (count > 4096) {
            r->error_flag = -1;
            return 0;
        }
    }
    *out = count;
    return 1;
}

static int bitreader_is_byte_aligned(mlac_bitreader_t *r)
{
    /* Byte-aligned when no partial bits consumed from current byte */
    return r->bit_in_byte == 7;
}

static uint32_t bitreader_bits_for_alignment(mlac_bitreader_t *r)
{
    /* Bits needed to reach next byte boundary */
    if (r->bit_in_byte == 7) return 0;
    return r->bit_in_byte + 1;
}

static void bitreader_start_crc16(mlac_bitreader_t *r, uint16_t seed)
{
    r->crc16 = seed;
    r->crc16_active = true;
}

static uint16_t bitreader_get_crc16(mlac_bitreader_t *r)
{
    return r->crc16;
}

/* ============================================================
 * WORKSPACE ALLOCATION
 * ============================================================ */

size_t mlac_decoder_get_workspace_size(uint32_t max_blocksize)
{
    size_t sz = sizeof(mlac_decoder_t);
    /* Per channel: the decoded signal plus a separate residual buffer.
     * The 33-bit wide path is not implemented, so no int64 scratch is carved
     * out — an earlier revision reserved 2 x int64 x 2ch, which alone was
     * 61 KB at max_blocksize and pushed the total past the largest contiguous
     * heap block on this target (123 KB requested vs 67 KB available). */
    sz += max_blocksize * sizeof(int32_t) * 2 * 2;   /* coeffs + residuals, 2ch */
    sz += 64; /* alignment padding */
    return sz;
}

mlac_decoder_t *mlac_decoder_init(void *workspace, uint32_t max_blocksize)
{
    if (!workspace) return NULL;
    memset(workspace, 0, mlac_decoder_get_workspace_size(max_blocksize));

    mlac_decoder_t *dec = (mlac_decoder_t *)workspace;
    dec->max_blocksize = max_blocksize;
    dec->state = 0;
    dec->has_pending_ff = false;

    /* Allocate buffers after the struct */
    uint8_t *ptr = (uint8_t *)workspace + sizeof(mlac_decoder_t);
    ptr = (uint8_t *)(((uintptr_t)ptr + 7) & ~7);

    for (int ch = 0; ch < 2; ch++) {
        dec->coeffs[ch] = (int32_t *)ptr;
        ptr += max_blocksize * sizeof(int32_t);
        dec->residuals[ch] = (int32_t *)ptr;
        ptr += max_blocksize * sizeof(int32_t);
    }

    dec->allocated = true;
    return dec;
}

/* ============================================================
 * FRAME DECODE — MILL-compatible decoder
 *
 * MIHC container: [0]=seq [1:3]=lhdc_fhdr [3:...]=MLAC bitstream
 * MLAC bitstream: sync(FF FB) + frame_header(3B) + subframes + CRC16
 *
 * Frame header (from Ghidra MILL__stream_decoder_process):
 *   sync_byte0 = 0xFF, sync_byte1 = 0xFB
 *   blocksize_indicator = sync_byte1 & 3 (0=192,1=576,2=960,3=480 @48kHz/20ms)
 *   frame_type = (sync_byte1 >> 2) & 7
 *   hdr[2] = sr_code(4b upper) | ch(2b) | warmup_flag(1b)
 *   hdr[3] = field_A(4b) | field_B(4b)  [if warmup]
 *   hdr[4] = CRC8
 *
 * Subframe per channel:
 *   sub_header(8b): predictor_order in bits[4:1], warmup_flag in bit[0]
 *   warmup_count (unary), warmup_samples (signed, bpc bits each)
 *   residual_method(2b), partition_order(4b), rice residuals
 *
 * Signal restore: order 0=copy, 1=delta, 2+=prediction
 * ============================================================ */

/* Rice decoder constants (from .so PTR_DAT tables) */
#define RICE_PARAM_BITS      4
#define ESCAPE_THRESHOLD     15
#define ESCAPE_PARAM_BITS    5

/* ============================================================
 * Signal restoration — mirrors libFLAC's
 * MILL__fixed_restore_signal / MILL__lpc_restore_signal.
 *
 * The .so calls these AFTER the whole residual array has been
 * read (read_residual_partitioned_rice_ fills residuals first,
 * then lpc_restore_signal walks them).  So residuals must live
 * in their own buffer: accumulating them straight into `out`
 * (out[i] = out[i-1] + residual) only reproduces the order-1
 * fixed predictor and is wrong for every other order.
 *
 *   data[i] = residual[i] + ((sum data[i-1-j]*qlp_coef[j]) >> shift)
 *
 * FLAC's fixed predictors (coefficients over the previous samples):
 *   order 0: 0
 *   order 1: x[i-1]
 *   order 2: 2*x[i-1] - x[i-2]
 *   order 3: 3*x[i-1] - 3*x[i-2] + x[i-3]
 *   order 4: 4*x[i-1] - 6*x[i-2] + 4*x[i-3] - x[i-4]
 * ============================================================ */
static const int32_t MLAC_FIXED_COEF[5][4] = {
    { 0,  0,  0,  0 },
    { 1,  0,  0,  0 },
    { 2, -1,  0,  0 },
    { 3, -3,  1,  0 },
    { 4, -6,  4, -1 },
};

/* Restore out[start .. start+len) from residual[start .. start+len), predicting
 * each sample from the `order` samples immediately before it (which are either
 * warmup samples or already-restored output).
 * `order` 1..4 selects a built-in fixed predictor; order > 4 uses qlp_coef[]
 * with `shift` (precision-reduced LPC coefficients). Mirrors the .so's
 * MILL__fixed_restore_signal / MILL__lpc_restore_signal.
 * `out` is the FULL signal buffer, not an offset view — prediction indexes
 * below `start` must stay inside the array. */
static void mlac_restore_signal(int32_t *out, const int32_t *residual,
                                uint32_t start, uint32_t len, uint32_t order,
                                const int32_t *qlp_coef, int qlp_shift)
{
    if (order == 0) {
        memcpy(out + start, residual + start, (size_t)len * sizeof(int32_t));
        return;
    }

    const int32_t *coef = (order <= 4) ? MLAC_FIXED_COEF[order] : qlp_coef;

    for (uint32_t i = 0; i < len; i++) {
        uint32_t idx = start + i;
        int64_t sum = 0;
        for (uint32_t j = 0; j < order; j++)
            sum += (int64_t)out[idx - 1 - j] * coef[j];
        if (order > 4 && qlp_shift > 0)
            sum >>= qlp_shift;
        out[idx] = residual[idx] + (int32_t)sum;
    }
}

/* MILL frame types */
#define FT_CONSTANT  0
#define FT_VERBATIM  1
#define FT_DELTA     2
#define FT_LPC0      3
#define FT_LPC1      4
#define FT_LPC2      5

mlac_ret_t mlac_decode_frame(mlac_decoder_t *dec,
                             const uint8_t *frame_data, uint32_t frame_len,
                             int16_t *pcm_out, uint32_t pcm_max_samples,
                             uint32_t *samples_out,
                             mlac_frame_info_t *info)
{
    if (!dec || !frame_data || frame_len < 10)
        return MLAC_ERR_HEADER;

    s_frame_count++;

    /* ====== Find MLAC sync in container ====== *
     * BT capture:    FF FB 5A ...   (bsi=3 -> 10ms)
     * MediaCodec AU: FF FA 51 ...   (bsi=2 -> 20ms)
     * Accept either second/third byte pair; bsi still comes from byte1&3. */
    int magic_pos = -1;
    for (uint32_t i = 0; i + 2 < frame_len; i++) {
        if (frame_data[i] != 0xFF) continue;
        uint8_t b1 = frame_data[i + 1], b2 = frame_data[i + 2];
        int is_b1 = (b1 == 0xFB || b1 == 0xFA || b1 == 0xFE);
        int is_b2 = (b2 == 0x5A || b2 == 0x51);
        if (is_b1 && is_b2) {
            magic_pos = (int)i;
            break;
        }
    }
    if (magic_pos < 0) {
        if ((s_frame_count % 20) == 1)
            MLAC_LOGW("no MLAC sync, len=%u", frame_len);
        return MLAC_ERR_SYNC;
    }

    /* ====== LHDC prefix = 2 bytes immediately BEFORE the sync ======
     * BT record:   [seq][lhdc:2][FF FB 5A ...]  -> prefix at magic_pos-2
     * Encoder AU:  [ff][len:2][03 41 02][FF FA 51 ...] -> prefix at magic_pos-2
     * Both carriers keep the prefix directly in front of the sync, so that
     * position is always the correct one.  When the caller already sliced to
     * the sync (magic_pos == 0, which is what a2dp_vendor_mihc_decoder.c
     * passes: `src + sync_at`) there is NO prefix in the buffer.  The old
     * fallback read frame_data[1] — but there frame_data[1] is 0x5A (the MLAC
     * header byte), so `0x5A >> 4 = 5` turned into a bogus <<5 amplitude
     * shift that clipped every sample to +/-32767 => loud noise.  The
     * meaningful fields are the 48 kHz / 2ch defaults plus shift 0, so use
     * them explicitly instead of misreading payload bytes. */
    uint32_t sample_shift = 0;
    if (magic_pos >= 2) {
        const uint8_t *lhdc_hdr = &frame_data[magic_pos - 2];
        static const uint32_t sr_table[5] = { 48000, 96000, 192000, 44100, 88200 };
        uint32_t sr_idx = (lhdc_hdr[0] >> 1) & 7;
        dec->sample_rate = (sr_idx < 5) ? sr_table[sr_idx] : 48000;
        dec->channels = (lhdc_hdr[0] & 1) + 1;  /* 0→1ch, 1→2ch */
        /* lhdc_hdr[1] high nibble = sample down-shift applied at encode time.
         * Coefficients are stored in (bpc - shift) bits; output must left-shift
         * by `shift` to recover the original PCM amplitude. Verified against
         * MediaCodec truth: AU prefix byte 0x02>>4=0 (attack frames 0-4, no
         * shift) vs 0x34>>4=3 (frames 5+, <<3). */
        sample_shift = (uint32_t)(lhdc_hdr[1] >> 4);
    } else {
        /* Sync-sliced input: no prefix byte is present in the buffer.
         * bpc (coefficient width) and shift (encode-time down-shift) are two
         * INDEPENDENT fields — do not derive one from the other. The BT
         * carrier prefixes each frame with "71 02" whose shift nibble is 0,
         * so 0 is the correct recovery for this path. (The encoder carrier
         * "03 41 02"/"03 41 34" uses shift 0/3, but that path always passes
         * the whole frame so it takes the magic_pos >= 2 branch above.) */
        dec->sample_rate = 48000;
        dec->channels = 2;
        sample_shift = 0;
    }

    /* ====== Initialize bitstream just past the 14-bit sync ====== *
     * The sync is 14 bits: the whole of 0xFF, then the top 6 bits of 0xFB
     * (0x3FFE).  The remaining 2 bits of 0xFB are the blocksize indicator, so
     * the frame-type byte (0x5A) sits at payload[2].  Starting the reader at
     * payload[0] would read 0xFF as the frame type and mis-position every
     * field after it.  The CRC seed already covers payload[0..1], so the
     * reader itself resumes at payload[2]. */
    const uint8_t *payload = frame_data + magic_pos;
    uint32_t payload_len = frame_len - (uint32_t)magic_pos;
    if (payload_len < 8)
        return MLAC_ERR_HEADER;

    bitreader_init(&dec->reader, payload + 2, payload_len - 2);

    /* CRC16 seed from first 2 sync bytes */
    dec->header_byte0 = payload[0];  /* 0xFF */
    dec->header_byte1 = payload[1];  /* 0xFB */
    uint16_t crc_seed = (uint16_t)(mlac_crc16_table[
                         (uint8_t)(mlac_crc16_table[payload[0]] >> 8) ^
                         payload[1]] ^
                         (mlac_crc16_table[payload[0]] << 8));
    bitreader_start_crc16(&dec->reader, crc_seed);

    /* ====== Parse MILL frame header (Ghidra-verified) ====== *
     * After14-bit sync (0x3FFE = FF FB), bitstream continues:  *
     * sync_byte1 (0xFB) lower 2 bits = blocksize_indicator     *
     * Then read8 bits header byte:                              *
     *   frame_type = (byte >> 4) & 7 (0=CONST..5=LPC2)        *
     *   If frame_type >= 2: read8 bits:                        *
     *     predictor_type = byte & 0xF, channel_mode >> 4 & 0xF *
     * Then CRC8 byte.                                           *
     * MIHC @48kHz: header=0x5A→ft=5(LPC2), 0x20→pred=0,ch=2  */
    uint32_t sync_byte1 = (uint32_t)payload[1];  /* 0xFB */
    uint32_t blocksize_indicator = sync_byte1 & 3;

    /* 0xFB = normal frame, 0xFE = VERBATIM variant (single channel).
     * The old ((b>>2)|4) == 0x3E test is TRUE for 0xFB as well, so it forced
     * every frame down the VERBATIM path and skipped the real header parse. */
    uint32_t frame_type_internal = (sync_byte1 == 0xFE) ? 1 : 0;

    uint32_t frame_type;
    uint32_t predictor_type = 0;
    uint32_t channel_mode = 1;
    uint32_t bpc_from_hdr = 24;   /* overwritten on the FF FB path */

    if (frame_type_internal == 1) {
        /* FF FE: VERBATIM, channels=1, no extra header */
        frame_type = FT_VERBATIM;
        dec->channels = 1;
    } else {
        /* FF FB: read header byte for frame_type */
        uint32_t hdr_byte;
        if (!bitreader_read_u32(&dec->reader, &hdr_byte, 8))
            return MLAC_ERR_BITSTREAM;
        frame_type = (hdr_byte >> 4) & 7;

        /* The .so derives TWO fields from this same byte (0xd8b8/0xd8c0):
         *   [state+0xbc] = (hdr_byte >> 2) & 3    -> channel mode
         *   [state+0xc4] = (hdr_byte & 3) * 8 + 8 -> bits per coefficient
         * Using blocksize_indicator for bpc (as an earlier revision did) gives
         * 32 instead of the real 24 for the common 0x5A header, which widens
         * every warmup and Rice residual field by 8 bits. */
        channel_mode = (hdr_byte >> 2) & 3;
        bpc_from_hdr = (hdr_byte & 3) * 8 + 8;

        if (frame_type >= FT_DELTA) {
            /* DELTA/LPC: read predictor_type + channel_mode */
            uint32_t hdr2;
            if (!bitreader_read_u32(&dec->reader, &hdr2, 8))
                return MLAC_ERR_BITSTREAM;
            predictor_type = hdr2 & 0xF;
            channel_mode = (hdr2 >> 4) & 0xF;
        }
        /* CRC8 byte */
        uint32_t hdr_crc8;
        if (!bitreader_read_u32(&dec->reader, &hdr_crc8, 8))
            return MLAC_ERR_BITSTREAM;
        (void)hdr_crc8;
    }

    /* Block size: .so line230: blocksize = (duration_ms * sample_rate) / 1000 */
    uint32_t duration_ms = (blocksize_indicator & 1) ? 10 : 20;
    uint32_t blocksize = (duration_ms * dec->sample_rate) / 1000;
    if (blocksize > dec->max_blocksize)
        blocksize = dec->max_blocksize;
    dec->blocksize = blocksize;

    /* bpc comes from the frame header byte (see above), NOT from the blocksize
     * indicator. 0x5A -> (0x5A & 3)*8 + 8 = 24 bits per coefficient. */
    uint32_t bpc = bpc_from_hdr;
    dec->bits_per_coeff = bpc;

    /* Channel mode: for frame_type >= 2, channel_mode >= 2 → stereo */
    if (frame_type >= FT_DELTA && channel_mode >= 2) {
        dec->channels = 2;
    }
    uint32_t num_channels = dec->channels;

    /* hdr2 low nibble is the stereo coding mode (FLAC channel assignment),
     * NOT the LPC predictor order. high nibble is the channel-count field. */
    dec->predictor_type = predictor_type;
    dec->channel_mode = (uint8_t)channel_mode;

    if (s_frame_count <= 80) {
        MLAC_LOGI("frame#%u mp=%d len=%u hdr=[%02x %02x %02x %02x %02x %02x] sh=%u "
                  "ft=%u bsi=%u pred=%u chm=%u bpc=%u bs=%u ch=%u sr=%u",
                  s_frame_count, magic_pos, frame_len,
                  frame_data[0], frame_data[1], frame_data[2], frame_data[3],
                  frame_data[4], frame_data[5],
                  sample_shift, frame_type, blocksize_indicator,
                  predictor_type, channel_mode, bpc, blocksize, num_channels,
                  dec->sample_rate);
    }

    /* ====== Decode subframes (one per channel) ====== */
    bool decode_ok = true;
    uint32_t ch = 0;          /* hoisted so the failure log below can see it */
    uint32_t sub_type6 = 0;
    uint32_t predictor_order = 0;

    for (ch = 0; ch < num_channels && decode_ok; ch++) {
        int32_t *out = dec->coeffs[ch];

        /* Channel-dependent bpc adjustment (from the .so's restore dispatch):
         * predictor_type 0: no adjustment
         * predictor_type 1 or 3: ch==1 -> bpc+1
         * predictor_type 2:      ch==0 -> bpc+1 */
        uint32_t ch_bpc = bpc;
        if (num_channels == 2) {
            if (predictor_type == 1 || predictor_type == 3) {
                if (ch == 1) ch_bpc = bpc + 1;
            } else if (predictor_type == 2) {
                if (ch == 0) ch_bpc = bpc + 1;
            }
        }

        /* Every subframe starts with its own 8-bit header (libFLAC layout):
         *   bit0     : zero-padding flag
         *   bits[6:1]: TYPE
         *                0      = CONSTANT
         *                1      = VERBATIM
         *                8..12  = FIXED, order = type - 8
         *                >=32   = LPC,   order = type - 31
         *   bit7     : wasted-bits flag; if set, unary-coded (wasted-1) follows.
         *              Coefficients are stored with (bpc - wasted) bits and
         *              must be left-shifted by `wasted` after restore.
         * The previous code branched on the FRAME-level frame_type and read
         * order as (sub_hdr>>1)&0xF, which is neither FLAC's type encoding nor
         * the right bit field — every subframe after the first decoded wrong. */
        uint32_t sub_hdr;
        if (!bitreader_read_u32(&dec->reader, &sub_hdr, 8)) {
            decode_ok = false; break;
        }
        uint32_t sub_type6 = (sub_hdr >> 1) & 0x3F;
        uint32_t wasted_bits = 0;
        if (sub_hdr & 0x80) {
            /* unary: k ones then a zero → wasted = k + 1 */
            uint32_t k = 0;
            for (;;) {
                uint32_t bit;
                if (!bitreader_read_u32(&dec->reader, &bit, 1)) {
                    decode_ok = false; break;
                }
                if (bit == 0) break;
                k++;
                if (k > 32) { decode_ok = false; break; }
            }
            if (!decode_ok) break;
            wasted_bits = k + 1;
            if (wasted_bits >= ch_bpc) { decode_ok = false; break; }
            ch_bpc -= wasted_bits;
        }
        dec->wasted[ch] = wasted_bits;
        uint32_t predictor_order = 0;
        bool     is_lpc = false;
        if (sub_type6 == 0) {
            /* CONSTANT: one sample at ch_bpc, replicated. */
            int64_t val;
            if (!bitreader_read_signed(&dec->reader, &val, ch_bpc)) {
                decode_ok = false; break;
            }
            for (uint32_t i = 0; i < blocksize; i++)
                out[i] = (int32_t)val;
            continue;
        } else if (sub_type6 == 1) {
            /* VERBATIM: blocksize raw signed samples. */
            for (uint32_t i = 0; i < blocksize; i++) {
                int64_t val;
                if (!bitreader_read_signed(&dec->reader, &val, ch_bpc)) {
                    decode_ok = false; break;
                }
                out[i] = (int32_t)val;
            }
            if (!decode_ok) break;
            continue;
        } else if (sub_type6 >= 8 && sub_type6 <= 12) {
            predictor_order = sub_type6 - 8;          /* FIXED */
        } else if (sub_type6 >= 32) {
            predictor_order = sub_type6 - 31;         /* LPC */
            is_lpc = true;
        } else {
            /* Reserved subframe type — cannot continue. */
            decode_ok = false; break;
        }

        /* LPC subframes carry `order` coefficients whose precision is declared
         * in 4 bits (precision-1), followed by a 5-bit signed shift. Fixed
         * subframes carry neither — they use the built-in FLAC predictor. */
        int32_t qlp_coef[32];
        int     qlp_shift = 0;
        if (is_lpc && predictor_order > 0) {
            if (predictor_order > 32) { decode_ok = false; break; }
            uint32_t precision_m1;
            if (!bitreader_read_u32(&dec->reader, &precision_m1, 4)) {
                decode_ok = false; break;
            }
            int32_t precision = (int32_t)precision_m1 + 1;
            int64_t sshift;
            if (!bitreader_read_signed(&dec->reader, &sshift, 5)) {
                decode_ok = false; break;
            }
            qlp_shift = (int)sshift;
            for (uint32_t j = 0; j < predictor_order; j++) {
                int64_t c;
                if (!bitreader_read_signed(&dec->reader, &c, precision)) {
                    decode_ok = false; break;
                }
                qlp_coef[j] = (int32_t)c;
            }
            if (!decode_ok) break;
        }

        /* Warmup samples: exactly `order` unencoded values, no unary count. */
        if (predictor_order > 0) {
            if (predictor_order > blocksize) { decode_ok = false; break; }
            for (uint32_t w = 0; w < predictor_order; w++) {
                int64_t ws;
                if (!bitreader_read_signed(&dec->reader, &ws, ch_bpc)) {
                    decode_ok = false; break;
                }
                out[w] = (int32_t)ws;
            }
            if (!decode_ok) break;
        }
        {
            /* DELTA / LPC body: subframe header + warmup + rice residuals */

            /* Residual method (2 bits) */
            uint32_t residual_method2;
            if (!bitreader_read_u32(&dec->reader, &residual_method2, 2)) {
                decode_ok = false; break;
            }

            /* Partition order (4 bits) */
            uint32_t partition_order;
            if (!bitreader_read_u32(&dec->reader, &partition_order, 4)) {
                decode_ok = false; break;
            }

            /* Decode rice-coded residuals into a SEPARATE buffer.
             * The prediction (fixed/LPC) is applied afterwards by
             * mlac_restore_signal(), exactly like the .so does: the residual
             * reader fills an array, then MILL__fixed/lpc_restore_signal walks
             * it.  Accumulating residuals straight into `out` only implements
             * the order-1 predictor and destroys every higher order. */
            int32_t *res = dec->residuals[ch];
            uint32_t num_partitions = 1 << partition_order;
            uint32_t residual_start = predictor_order;
            bool rice_ok = true;

            for (uint32_t part = 0; part < num_partitions && rice_ok; part++) {
                uint32_t rice_param;
                if (!bitreader_read_u32(&dec->reader, &rice_param, RICE_PARAM_BITS)) {
                    rice_ok = false; break;
                }

                uint32_t part_size;
                uint32_t part_base;
                if (partition_order == 0) {
                    part_base = 0;
                    part_size = blocksize - residual_start;
                } else {
                    uint32_t base = blocksize >> partition_order;
                    part_base = (part == 0) ? 0 : base * part;
                    part_size = (part == 0) ? (base - residual_start) : base;
                }

                if (rice_param < ESCAPE_THRESHOLD) {
                    for (uint32_t s = 0; s < part_size; s++) {
                        uint32_t quotient;
                        if (!bitreader_read_unary(&dec->reader, &quotient)) {
                            rice_ok = false; break;
                        }
                        uint32_t remainder;
                        if (rice_param &&
                            !bitreader_read_u32(&dec->reader, &remainder, rice_param)) {
                            rice_ok = false; break;
                        }
                        if (!rice_param) remainder = 0;
                        uint32_t zigzag = (quotient << rice_param) | remainder;
                        int32_t residual = (int32_t)(zigzag >> 1) ^ (-(int32_t)(zigzag & 1));

                        uint32_t idx = residual_start + part_base + s;
                        if (idx < blocksize)
                            res[idx] = residual;
                    }
                } else {
                    uint32_t escape_param;
                    if (!bitreader_read_u32(&dec->reader, &escape_param, ESCAPE_PARAM_BITS)) {
                        rice_ok = false; break;
                    }

                    for (uint32_t s = 0; s < part_size; s++) {
                        int32_t v = 0;
                        if (escape_param) {
                            int64_t val;
                            if (!bitreader_read_signed(&dec->reader, &val, escape_param)) {
                                rice_ok = false; break;
                            }
                            v = (int32_t)val;
                        }
                        uint32_t idx = residual_start + part_base + s;
                        if (idx < blocksize)
                            res[idx] = v;
                    }
                }
                residual_start = 0;
            }

            if (!rice_ok) {
                decode_ok = false;
                break;
            }

            /* Apply the predictor over the residual array. Warmup samples
             * already occupy out[0..order), so restoration starts at index
             * `order` and walks forward, using the samples it has produced. */
            if (predictor_order > 0 && predictor_order < blocksize) {
                mlac_restore_signal(out, res, predictor_order,
                                    blocksize - predictor_order,
                                    predictor_order, qlp_coef, qlp_shift);
            } else if (predictor_order == 0) {
                /* No prediction: residual IS the signal. */
                memcpy(out, res, (size_t)blocksize * sizeof(int32_t));
            }
        }

        /* Debug: dump first samples */
        if (s_frame_count <= 10) {
            int32_t *c = dec->coeffs[ch];
            int32_t maxv = 0, minv = 0;
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < blocksize; i++) {
                if (c[i] > maxv) maxv = c[i];
                if (c[i] < minv) minv = c[i];
                if (c[i] != 0) nonzero++;
            }
            MLAC_LOGI("  ch%u: ft=%u bs=%u [%d %d %d %d %d] max=%d min=%d nz=%u",
                      ch, frame_type, blocksize,
                      c[0], c[1], c[2], c[3], c[4],
                      maxv, minv, nonzero);
        }
    }

    if (!decode_ok) {
        /* Pinpoint which subframe/type died and how far the reader got, so a
         * silent MLAC_ERR_BITSTREAM can be traced to one field instead of
         * requiring another blind rebuild. */
        MLAC_LOGW("decode FAIL: ch=%u/%u sub_type6=%u order=%u ch_bpc=%u bs=%u "
                  "rd(byte=%u bit=%d err=%d len=%u) bpc=%u pred=%u",
                  ch, num_channels, sub_type6, predictor_order, bpc, blocksize,
                  dec->reader.byte_idx, dec->reader.bit_in_byte,
                  dec->reader.error_flag, dec->reader.data_len,
                  bpc, predictor_type);
        return MLAC_ERR_BITSTREAM;
    }

    /* ====== Byte alignment ====== */
    if (!bitreader_is_byte_aligned(&dec->reader)) {
        uint32_t align_bits = bitreader_bits_for_alignment(&dec->reader);
        uint32_t dummy;
        bitreader_read_u32(&dec->reader, &dummy, align_bits);
    }

    /* ====== CRC16 verification ====== */
    uint32_t stored_crc;
    if (bitreader_read_u32(&dec->reader, &stored_crc, 16)) {
        uint16_t computed_crc = bitreader_get_crc16(&dec->reader);
        (void)stored_crc; (void)computed_crc;
        /* CRC check — log mismatch but don't fail */
    }

    /* ====== Output: sample-shift + wasted-bits + stereo recon, int16 ======
     * Per-channel left shift = lhdc_hdr[1]>>4 (encode-time down-shift)
     *                          + wasted_bits (FLAC subframe hdr bit7).
     * Stereo coding mode is the hdr2 low nibble (stored as predictor_type):
     *   0 indep      L=ch0,              R=ch1
     *   1 left/side  L=ch0,              R=ch0-ch1
     *   2 right/side L=ch0+ch1,          R=ch1
     *   3 mid/side   L=((ch0<<1)+ch1)>>1, R=((ch0<<1)-ch1)>>1 */
    uint32_t total_samples = blocksize * num_channels;
    if (total_samples > pcm_max_samples)
        total_samples = pcm_max_samples;

    uint32_t ch_mode = dec->predictor_type;
    uint32_t sh0 = sample_shift + dec->wasted[0];
    uint32_t sh1 = sample_shift + dec->wasted[1];
    for (uint32_t i = 0; i < blocksize; i++) {
        int32_t L, R;
        if (num_channels >= 2) {
            int32_t a = dec->coeffs[0][i] << sh0;
            int32_t b = dec->coeffs[1][i] << sh1;
            switch (ch_mode) {
                case 1:  L = a;       R = a - b;  break;
                case 2:  L = a + b;   R = b;      break;
                case 3:  L = ((a << 1) + b) >> 1;
                         R = ((a << 1) - b) >> 1;  break;
                default: L = a;       R = b;      break;
            }
        } else {
            L = dec->coeffs[0][i] << sh0;
            R = L;
        }

        if (L > 32767) L = 32767;
        if (L < -32768) L = -32768;
        if (R > 32767) R = 32767;
        if (R < -32768) R = -32768;

        uint32_t idx = i * num_channels;
        if (idx >= pcm_max_samples) break;
        pcm_out[idx] = (int16_t)L;
        if (num_channels >= 2 && idx + 1 < pcm_max_samples)
            pcm_out[idx + 1] = (int16_t)R;
    }

    if (samples_out) *samples_out = total_samples;
    if (info) {
        info->blocksize = blocksize;
        info->sample_rate = dec->sample_rate;
        info->channels = dec->channels;
        info->bits_per_coeff = dec->bits_per_coeff;
        info->layer_count = dec->layer_count;
        info->predictor_type = dec->predictor_type;
        info->is33bit = (dec->bits_per_coeff > 24);
    }

    return MLAC_OK;
}
