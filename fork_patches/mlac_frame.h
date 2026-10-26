/*
 * MLAC frame parser — extracts payload from over-the-air MIHC frames.
 *
 * Real captured frame layout (72 frames, all 626 bytes, 48kHz stereo):
 *
 *   Offset  Size  Field           Value (example)
 *   [0]     1B    seq_marker      0x44,0x45,0x46... (incrementing)
 *   [1:3]   2B    lhdc_fhdr       0x0271 LE (always same)
 *   [3:7]   4B    magic           FF FB5A 20
 *   [7]     1B    frame_type      0x3F (always same)
 *   [8:10]  2B    payload_size    0x0266 = 614 LE
 *   [11:N]  614B  payload         MLAC compressed data (high entropy)
 *   [N:N+2] 2B    trailer         variable (CRC or checksum)
 *   Total:  626B
 *
 * The first3 bytes are the "LHDC adaptation layer" framing inherited from
 * the A2DP media transport. The MLAC container starts at offset3.
 *
 * Based on: test_mihc.c frame parsing + mihc_real_frames.txt analysis.
 */

#ifndef MLAC_FRAME_H
#define MLAC_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MLAC magic bytes at container offset [0:4] */
#define MLAC_MAGIC_0  0xFF
#define MLAC_MAGIC_1  0xFB
#define MLAC_MAGIC_2  0x5A
#define MLAC_MAGIC_3_20  0x20   /* normal frame */
#define MLAC_MAGIC_3_3F  0x3F   /* alternate frame type */

/* Frame header sizes */
#define MLAC_SEQ_MARKER_SIZE   1
#define MLAC_LHDC_FHDR_SIZE    2
#define MLAC_CONTAINER_OFFSET  (MLAC_SEQ_MARKER_SIZE + MLAC_LHDC_FHDR_SIZE)  /* =3 */
#define MLAC_CONTAINER_MAGIC   4
#define MLAC_CONTAINER_TYPE    1
#define MLAC_CONTAINER_SIZE    2
/*
 * Payload starts at offset11 (confirmed by test_mihc.c: payload = raw + 11).
 *
 * The11-byte container header [0:11]:
 *   [0]     seq_marker (1B)
 *   [1:3]   lhdc_fhdr (2B)
 *   [3:7]   magic FF FB5A 20 (4B)
 *   [7]     frame_type 0x3F (1B)
 *   [8:10]  payload_size LE u16 (2B, always614)
 *   [10]    reserved (1B, always 0xF4)
 *
 * After header: [11:11+614] = payload, [11+614:] =2-byte trailer.
 */
#define MLAC_PAYLOAD_OFFSET    11
#define MLAC_HEADER_TOTAL      MLAC_PAYLOAD_OFFSET  /* =11 */

/* Maximum MLAC payload size (sanity bound) */
#define MLAC_MAX_PAYLOAD_SIZE  2048

/* Parsed MLAC frame */
typedef struct {
    uint8_t  seq_marker;        /* [0]: sequence counter */
    uint8_t  frame_type;        /* [7]: container frame type (0x3F) */
    uint16_t payload_size;      /* [8:10]: LE u16, typically 614 */
    const uint8_t *payload;     /* pointer into input buffer at [11] */
    uint16_t payload_len;       /* actual bytes available */
    const uint8_t *trailer;     /* pointer to2-byte trailer */
} mlac_frame_t;

/*
 * Parse one MLAC frame from raw A2DP payload.
 *
 * in_data:  raw bytes starting at the seq marker byte [0]
 * in_bytes: total bytes available
 * frame:    [out] parsed frame info
 *
 * Returns: number of bytes consumed (including trailer), or 0 on error.
 */
static inline size_t mlac_frame_parse(const uint8_t *in_data, size_t in_bytes,
                                      mlac_frame_t *frame)
{
    if (!in_data || !frame || in_bytes < MLAC_HEADER_TOTAL + 2) {
        return 0;
    }

    /* Verify magic bytes at [3:7] */
    if (in_data[3] != MLAC_MAGIC_0 ||
        in_data[4] != MLAC_MAGIC_1 ||
        in_data[5] != MLAC_MAGIC_2) {
        return 0;  /* not an MLAC frame */
    }

    frame->seq_marker   = in_data[0];
    frame->frame_type   = in_data[7];
    frame->payload_size = (uint16_t)(in_data[8] | (in_data[9] << 8));

    /* Payload starts at offset11 (confirmed by test_mihc.c) */
    if (in_bytes <= MLAC_PAYLOAD_OFFSET) return 0;

    size_t avail = in_bytes - MLAC_PAYLOAD_OFFSET;
    frame->payload     = in_data + MLAC_PAYLOAD_OFFSET;
    frame->payload_len = (uint16_t)(avail < frame->payload_size ? avail : frame->payload_size);
    frame->trailer     = in_data + MLAC_PAYLOAD_OFFSET + frame->payload_len;

    /* Total consumed: header + payload_size + 2 (trailer) */
    size_t total = MLAC_PAYLOAD_OFFSET + (size_t)frame->payload_size + 2;
    return total <= in_bytes ? total : in_bytes;
}

#ifdef __cplusplus
}
#endif

#endif /* MLAC_FRAME_H */
