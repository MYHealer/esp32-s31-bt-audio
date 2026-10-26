/*
 * MLAC decoder — rewritten from Ghidra decompilation of MILL__stream_decoder_process
 * in libcodec2_soft_mihc_dec.so
 *
 * Key algorithm (confirmed by .so disassembly):
 *   1. Word-addressed bitstream reader (big-endian u32 words, MSB-first)
 *   2. Frame sync: 0xFF + config byte validated by (config>>2)|4 == 0x3E
 *   3. CRC16/ARC bitstream integrity (polynomial 0x8005)
 *   4. Sub-frames: control byte -> silence | signed N-bit coefficients
 *   5. Fixed prediction (orders 0-4) or LPC prediction
 *   6. Multi-layer (LL+LY) interleaving
 */

#ifndef MLAC_DECODE_H
#define MLAC_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest block this decoder must hold: 1920 covers 96 kHz / 20 ms. The
 * workspace lives in PSRAM (see MIHC_WS_CAPS), so reserving the full size costs
 * no internal RAM — shrinking it is not needed and would cap the frame rate. */
#define MLAC_MAX_BLOCKSIZE    1920
#define MLAC_MAX_CHANNELS     2

typedef enum {
    MLAC_OK = 0,
    MLAC_ERR_SYNC,
    MLAC_ERR_HEADER,
    MLAC_ERR_CRC,
    MLAC_ERR_BITSTREAM,
    MLAC_ERR_NOMEM,
    MLAC_ERR_RESIDUAL,
} mlac_ret_t;

/* Bitstream reader — byte-based MSB-first, defined locally in mlac_decode.c
 * but declared here so mlac_decoder_t can embed it. */
typedef struct {
    const uint8_t *data;
    uint32_t data_len;
    uint32_t byte_idx;
    int      bit_in_byte;   /* 7..0, -1 = byte exhausted */
    int      error_flag;    /* -1 = error */
    uint16_t crc16;
    bool     crc16_active;
} mlac_bitreader_t;

/* Frame info after header parse */
typedef struct {
    uint32_t blocksize;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_coeff;
    uint8_t  layer_count;
    uint8_t  predictor_type;
    bool     is33bit;
} mlac_frame_info_t;

/* Decoder workspace */
typedef struct {
    uint8_t  state;
    uint8_t  header_byte0;
    uint8_t  header_byte1;
    bool     has_pending_ff;

    uint32_t blocksize;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  channel_mode;
    uint8_t  bits_per_coeff;
    uint8_t  layer_count;
    uint8_t  duration_ms;
    uint32_t predictor_type;

    mlac_bitreader_t reader;  /* embedded bitstream reader */

    int32_t  *coeffs[MLAC_MAX_CHANNELS];      /* decoded signal per channel */
    int32_t  *residuals[MLAC_MAX_CHANNELS];   /* Rice residuals, pre-prediction */
    uint32_t  wasted[MLAC_MAX_CHANNELS];      /* per-subframe wasted-bits shift */

    uint32_t max_blocksize;
    bool     allocated;
} mlac_decoder_t;

size_t mlac_decoder_get_workspace_size(uint32_t max_blocksize);
mlac_decoder_t *mlac_decoder_init(void *workspace, uint32_t max_blocksize);

mlac_ret_t mlac_decode_frame(mlac_decoder_t *dec,
                             const uint8_t *frame_data, uint32_t frame_len,
                             int16_t *pcm_out, uint32_t pcm_max_samples,
                             uint32_t *samples_out,
                             mlac_frame_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* MLAC_DECODE_H */
