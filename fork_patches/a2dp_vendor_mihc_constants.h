#ifndef A2DP_VENDOR_MIHC_CONSTANTS_H
#define A2DP_VENDOR_MIHC_CONSTANTS_H

/* MIHC (Xiaomi High-quality Codec) is a Xiaomi-proprietary vendor codec that
 * rides the same AVDT NON_A2DP vendor-codec framework as LHDC V5.
 *
 * vendor_id / codec_id were recovered from the official Redmi Buds 8 Pro
 * firmware (1.2.3.6, CN) codec-descriptor table at VA 0x2c393f70 / 0x2c395dc3:
 *
 *   bytes: 8f 03 00 00 | 31 4d | 01 00
 *          \__vendor__/   \_codec_/
 *
 * The same table yields the already-public LHDC V5 (0x053a / 0x4c35) and LDAC
 * (0x012d / 0x00aa) pairs verbatim, which validates the extraction format.
 *
 * codec_id 0x4d31 spells "M1" in ASCII, matching Savitech's naming convention
 * (LHDC V5 = 0x4c35 -> "L5"), and matches the MIHC1 library version v1.2.1
 * found in the same firmware. MIHC2 (v2.1.3) would presumably be 0x4d32.
 */
#define A2DP_MIHC_VENDOR_ID 0x0000038f
#define A2DP_MIHC_CODEC_ID  0x4d31

/* MIHC capability length on the wire.
 *
 * The phone side (MIUI libbluetooth_jni.so, a2dp_vendor_mihc.cc) parses the
 * SEP capability and hard-rejects any first byte that is not 0x0F:
 *
 *     0xa47fb0  ldrb w19,[x1]          ; cap[0]
 *     0xa47fb4  cmp  w19,#0xf
 *     0xa47fb8  b.ne <"wrong length">  ; anything else -> reject, return 13
 *
 * so the length byte MUST be 15, not 13. The old 13 came from the BES earphone
 * firmware's own SEP descriptor (a2dp_codec_mihc_common_init), which uses a
 * shorter envelope than what the phone accepts over the air.
 *
 * The same routine walks the capability at these fixed offsets; every mask is an
 * `ands ... ; b.eq <return 13>` guard, so a zero anywhere fails the whole parse:
 *
 *   cap[0]    == 0x0F              length
 *   cap[1]    <= 0x0F              media_type << 4   (media_type must be 0)
 *   cap[2]    == 0xFF              codec_type NON_A2DP
 *   cap[3..6] == 8F 03 00 00       vendor_id LE
 *   cap[7..8] == 31 4D             codec_id  LE
 *   cap[9]    != 0
 *   cap[10]   != 0
 *   cap[11]   bit0                 "codecSpecific3" flag read by the APK
 *             & 0x0C != 0          sample-rate class
 *             & 0xE0 != 0          bit-depth / adaptive-level class
 *   cap[12]   & 0x03 != 0          frame-length class (low)
 *             & 0xF0 != 0          frame-length class (high)
 *   cap[13]   != 0
 *   cap[14]   & 0x0F != 0
 */
#define A2DP_MIHC_CODEC_LEN 15

/* MIHC carries media_type 0, not A2D_MEDIA_TYPE_AUDIO (which is also 0x00 here,
 * but this name makes the MIHC-specific intent explicit at the call sites).
 * cap[1] is checked as `<= 0x0F`, i.e. the whole high nibble must be zero. */
#define A2DP_MIHC_MEDIA_TYPE 0x00

/* Sampling-frequency bitmap. Verified by disassembling the original firmware's
 * select_mihc_cap() (block7 offset 0xad6c): it tests the peer/local AND result
 * with the standard A2DP SBC bit positions and loads the matching Hz value:
 *   lsls #0x1e -> 48000 (0xbb80), mask 0x04
 *   lsls #0x1d -> 44100 (0xac44), mask 0x08
 *   lsls #0x1c -> 32000 (0x7d00), mask 0x10
 *   lsls #0x1b -> 24000 (0x5dc0), mask 0x20
 *   lsls #0x1a -> 16000 (0x3e80), mask 0x40
 * so the encoding follows the A2DP SBC convention, not the LHDC V5 one. */
#define A2DP_MIHC_SAMPLING_FREQ_MASK   0x7c
#define A2DP_MIHC_SAMPLING_FREQ_48000  0x04
#define A2DP_MIHC_SAMPLING_FREQ_44100  0x08
#define A2DP_MIHC_SAMPLING_FREQ_32000  0x10
#define A2DP_MIHC_SAMPLING_FREQ_24000  0x20
#define A2DP_MIHC_SAMPLING_FREQ_16000  0x40

/* Channel mode (select_mihc_cap: local[9] & 3, AND peer[0xd]). */
#define A2DP_MIHC_CHANNEL_MODE_MONO   0x01
#define A2DP_MIHC_CHANNEL_MODE_STEREO 0x02

/* Value the BES firmware's select_mihc_cap() originally used for the
 * "adaptive level 4" setting. In the 15-byte envelope this same value now lives
 * in cap[11] as the bit-depth class, i.e. it is numerically identical to
 * A2DP_MIHC_DEPTHCLASS_16 (0x40) - which is exactly what the 48 kHz / 16-bit
 * configuration needs. Kept under the historical name because
 * A2DP_VendorBuildCodecConfigMihc() echoes it back. */
#define A2DP_MIHC_CH_ADPT_LVL_4 0x40

/* Bit depth (select_mihc_cap: local[0xb] bit7 & peer[0xf] bit7). The BES
 * capability serializer exposes bit depth in the high nibble of its trailing
 * 16-bit field as 8/4/2, with 16-bit being the value the stock firmware uses. */
#define A2DP_MIHC_BIT_FMT_MASK 0x07
#define A2DP_MIHC_BIT_FMT_24   0x02
#define A2DP_MIHC_BIT_FMT_16   0x04

#define A2DP_MIHC_BITS_PER_SAMPLE_24 A2DP_MIHC_BIT_FMT_24
#define A2DP_MIHC_BITS_PER_SAMPLE_16 A2DP_MIHC_BIT_FMT_16

/* cap[1] low nibble on the wire; the phone parser only requires the high nibble
 * of cap[1] to be zero, so any value below 0x10 is accepted. */
#define A2DP_MIHC_VER_1        0x01

/* ---------------------------------------------------------------------------
 * The three "class bitmap" bytes the phone parses with per-bit masks.
 *
 * cap[11] is overloaded: the phone reads it three times, once per sub-field.
 * The bit positions below are taken verbatim from that parser:
 *
 *   0xa48088  and  w?,cap[11],#0x01   -> "codecSpecific3" (APK reads this == 1)
 *   0xa480ec  ands w?,cap[11],#0x0C   must be non-zero
 *   0xa48100  ands w?,cap[11],#0xE0   must be non-zero
 *   0xa48110  ands w?,cap[12],#0x03   must be non-zero
 *   0xa48120  ands w?,cap[12],#0xF0   must be non-zero
 *   0xa481e0  ands w?,cap[14],#0x0F   must be non-zero
 *
 * Semantically these are "which classes of X do I support" bitmaps, in the same
 * spirit as the A2DP SBC sampling-frequency bitmap, so each gets its own small
 * enum here. The concrete class-to-bit assignment below mirrors the SBC bit
 * ordering that the BES firmware's select_mihc_cap() uses (16/24 kHz upward),
 * and is chosen so that the 48 kHz / 16-bit / stereo combination we actually
 * negotiate always lands on a non-zero bit.
 * --------------------------------------------------------------------------- */
#define A2DP_MIHC_CAP11_FLAG_MASK     0x01
#define A2DP_MIHC_CAP11_SRCLASS_MASK  0x0C
#define A2DP_MIHC_CAP11_DEPTH_MASK    0xe0
#define A2DP_MIHC_CAP12_LOW_MASK      0x03
#define A2DP_MIHC_CAP12_HIGH_MASK     0xf0
#define A2DP_MIHC_CAP14_MASK          0x0f

/* cap[11] bit0 - the value the MIUI stack exposes as `codecSpecific3`. The APK
 * (MiuiBluetoothLatencyMode) selects MIHC only when this bit is set. */
#define A2DP_MIHC_LATENCY_SUPPORTED   0x01

/* cap[11] bits2-3: sample-rate class. 48 kHz is the one the I2S path runs at. */
#define A2DP_MIHC_SRCLASS_48000       0x04

/* cap[11] bits5-7: bit-depth / adaptive-level class. */
#define A2DP_MIHC_DEPTHCLASS_16       0x40

/* cap[12]: frame-length class. MIHC frames are 5 ms / 10 ms ("10ms" appears as
 * a literal in the phone's own MIHC builder). */
#define A2DP_MIHC_FRAMELEN_LOW        0x01
#define A2DP_MIHC_FRAMELEN_HIGH       0x10

/* cap[13]: channel bitmap (same bit convention as the SBC channel-mode field). */
#define A2DP_MIHC_CHANNEL_MASK        0x03

/* cap[14] bits0-3: trailing class nibble - must simply be non-zero. */
#define A2DP_MIHC_TRAILING_CLASS      0x01

#endif  // A2DP_VENDOR_MIHC_CONSTANTS_H
