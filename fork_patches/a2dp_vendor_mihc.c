/*
 * MIHC (Xiaomi High-quality Codec) vendor codec support.
 *
 * MIHC rides the AVDT NON_A2DP vendor codec framework and uses the same 13-byte
 * capability layout as LHDC V5, which is why this file is shaped after
 * a2dp_vendor_lhdcv5.c. The two codecs are NOT the same codec family, though -
 * the BES firmware's own MIHC decoder (see a2dp_vendor_mihc_decoder.c) shares
 * nothing with the LHDC V5 engine beyond the vendor-codec envelope.
 *
 * Two consequences:
 *
 *   1. A2DP_ParseInfoMihc() is lenient. The peer builds the MIHC capability
 *      bitmap at run time (the static image only carries the vendor_id /
 *      codec_id pair), so unlike LHDC we cannot validate the sample-rate and
 *      bit-depth fields against a known-good table. Rejecting a peer over an
 *      unrecognised bit pattern would break negotiation for a codec whose bytes
 *      we do not fully own.
 *
 *   2. MIHC is registered as a SEP but deliberately declines negotiation - see
 *      A2DP_VendorBuildCodecConfigMihc(). The decoder we can build on top of
 *      the LHDC engine cannot reconstruct MIHC's container framing, so letting
 *      the phone select MIHC would produce noise rather than PCM. Declining at
 *      SET_CONFIGURATION time makes the phone fall back to AAC / LHDC.
 */

#include <string.h>

#include "a2d_int.h"
#include "common/bt_defs.h"
#include "common/bt_target.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "stack/a2dp_vendor_mihc.h"
#include "stack/a2dp_vendor_mihc_decoder.h"

#if (defined(MIHC_DEC_INCLUDED) && MIHC_DEC_INCLUDED == TRUE)

static const tA2DP_MIHC_CIE a2dp_mihc_sink_caps = {
    A2DP_MIHC_VENDOR_ID,
    A2DP_MIHC_CODEC_ID,
    A2DP_MIHC_SAMPLING_FREQ_48000 |
        A2DP_MIHC_SAMPLING_FREQ_44100 |
        A2DP_MIHC_SAMPLING_FREQ_32000,
    A2DP_MIHC_BITS_PER_SAMPLE_16,
    A2DP_MIHC_CHANNEL_MODE_STEREO,
    A2DP_MIHC_CH_ADPT_LVL_4,
    A2DP_MIHC_VER_1,
};

/* The negotiated configuration the sink would prefer when several options are
 * mutually supported. Kept at 48 kHz / 16-bit stereo because that is what the
 * downstream I2S path (ES8389 at CODEC_DAC_SAMPLE_RATE) is built for. */
static const tA2DP_MIHC_CIE a2dp_mihc_default_config = {
    A2DP_MIHC_VENDOR_ID,
    A2DP_MIHC_CODEC_ID,
    A2DP_MIHC_SAMPLING_FREQ_48000,
    A2DP_MIHC_BITS_PER_SAMPLE_16,
    A2DP_MIHC_CHANNEL_MODE_STEREO,
    A2DP_MIHC_CH_ADPT_LVL_4,
    A2DP_MIHC_VER_1,
};

tA2D_STATUS A2DP_BuildInfoMihc(uint8_t media_type,
                               const tA2DP_MIHC_CIE* p_ie,
                               uint8_t* p_result) {
  if (p_ie == NULL || p_result == NULL) return A2D_INVALID_PARAMS;

  /* 15-byte MIHC capability. Field-by-field layout and the phone-side guards
   * that consume it are documented in a2dp_vendor_mihc_constants.h. */
  *p_result++ = A2DP_MIHC_CODEC_LEN;
  *p_result++ = (media_type << 4) | (p_ie->version & 0x0f);
  *p_result++ = A2D_MEDIA_CT_NON_A2DP;
  *p_result++ = (uint8_t)(p_ie->vendorId & 0xff);
  *p_result++ = (uint8_t)((p_ie->vendorId >> 8) & 0xff);
  *p_result++ = (uint8_t)((p_ie->vendorId >> 16) & 0xff);
  *p_result++ = (uint8_t)((p_ie->vendorId >> 24) & 0xff);
  *p_result++ = (uint8_t)(p_ie->codecId & 0xff);
  *p_result++ = (uint8_t)((p_ie->codecId >> 8) & 0xff);
  /* cap[9]: sample-rate bitmap (must be non-zero). */
  *p_result++ = p_ie->sampleRate & A2DP_MIHC_SAMPLING_FREQ_MASK;
  /* cap[10]: bit-depth bitmap (must be non-zero). */
  *p_result++ = p_ie->bitsPerSample & A2DP_MIHC_BIT_FMT_MASK;
  /* cap[11]: three overlaid sub-fields -
   *   bit0    latency/adaptive signalling (the APK's "codecSpecific3")
   *   bits2-3 sample-rate class  (must be non-zero)
   *   bits5-7 bit-depth class    (must be non-zero) */
  *p_result++ = A2DP_MIHC_LATENCY_SUPPORTED |
                (A2DP_MIHC_SRCLASS_48000 & A2DP_MIHC_CAP11_SRCLASS_MASK) |
                (A2DP_MIHC_DEPTHCLASS_16 & A2DP_MIHC_CAP11_DEPTH_MASK);
  /* cap[12]: frame-length class, both nibbles must be non-zero. */
  *p_result++ = (A2DP_MIHC_FRAMELEN_LOW & A2DP_MIHC_CAP12_LOW_MASK) |
                (A2DP_MIHC_FRAMELEN_HIGH & A2DP_MIHC_CAP12_HIGH_MASK);
  /* cap[13]: channel bitmap (must be non-zero). */
  *p_result++ = p_ie->channelMode & A2DP_MIHC_CHANNEL_MASK;
  /* cap[14]: trailing class nibble (must be non-zero). */
  *p_result++ = A2DP_MIHC_TRAILING_CLASS & A2DP_MIHC_CAP14_MASK;

  return A2D_SUCCESS;
}

tA2D_STATUS A2DP_ParseInfoMihc(tA2DP_MIHC_CIE* p_ie,
                               const uint8_t* p_codec_info,
                               bool is_capability) {
  if (p_ie == NULL || p_codec_info == NULL) return A2D_INVALID_PARAMS;
  memset(p_ie, 0, sizeof(*p_ie));

  uint8_t losc = *p_codec_info++;
  if (losc != A2DP_MIHC_CODEC_LEN) return A2D_WRONG_CODEC;

  uint8_t media_type = (*p_codec_info++) >> 4;
  tA2D_CODEC_TYPE codec_type = *p_codec_info++;
  if (media_type != A2D_MEDIA_TYPE_AUDIO ||
      codec_type != A2D_MEDIA_CT_NON_A2DP) {
    return A2D_WRONG_CODEC;
  }

  p_ie->vendorId = (uint32_t)p_codec_info[0] |
                   ((uint32_t)p_codec_info[1] << 8) |
                   ((uint32_t)p_codec_info[2] << 16) |
                   ((uint32_t)p_codec_info[3] << 24);
  p_codec_info += 4;
  p_ie->codecId = (uint16_t)p_codec_info[0] |
                  ((uint16_t)p_codec_info[1] << 8);
  p_codec_info += 2;

  if (p_ie->vendorId != A2DP_MIHC_VENDOR_ID ||
      p_ie->codecId != A2DP_MIHC_CODEC_ID) {
    return A2D_WRONG_CODEC;
  }

  p_ie->sampleRate = *p_codec_info++ & A2DP_MIHC_SAMPLING_FREQ_MASK;
  p_ie->bitsPerSample = *p_codec_info++ & A2DP_MIHC_BIT_FMT_MASK;
  /* cap[11] carrying three overlaid sub-fields; recover the ones the local
   * config cares about. */
  uint8_t c11 = *p_codec_info++;
  p_ie->chAdaptiveLevel = c11 & A2DP_MIHC_CAP11_DEPTH_MASK;
  /* cap[12], cap[13], cap[14] complete the 15-byte envelope. cap[13] is the
   * channel bitmap, in the same bit convention as the A2DP channel-mode field. */
  (void)*p_codec_info++;                       /* cap[12] frame-length class */
  p_ie->channelMode = *p_codec_info++ & A2DP_MIHC_CHANNEL_MASK;
  (void)*p_codec_info++;                       /* cap[14] trailing class */

  /* Deliberately no further validation: see the file header. The bits above
   * are echoed by the peer and may legitimately carry patterns the BES
   * firmware never emits. */
  (void)is_capability;

  return A2D_SUCCESS;
}

tA2D_STATUS A2DP_IsVendorPeerSourceCodecValidMihc(
    const uint8_t* p_codec_info) {
  tA2DP_MIHC_CIE cfg_cie;
  /* Accept either form: GET_CAPABILITIES carries a capability bitmap
   * (is_capability=true) while SET_CONFIGURATION carries a chosen
   * configuration (false). A2DP_ParseInfoMihc() does not distinguish the two,
   * so both paths succeed for a well-formed header; the retry mirrors the
   * LHDC V5 sibling so the two stay structurally identical. */
  tA2D_STATUS status = A2DP_ParseInfoMihc(&cfg_cie, p_codec_info, true);
  if (status == A2D_SUCCESS) return status;
  return A2DP_ParseInfoMihc(&cfg_cie, p_codec_info, false);
}

bool A2DP_IsVendorPeerSinkCodecValidMihc(const uint8_t* p_codec_info) {
  tA2DP_MIHC_CIE cfg_cie;
  bool valid = A2DP_ParseInfoMihc(&cfg_cie, p_codec_info, false) == A2D_SUCCESS;
  return (bool)valid;
}

btav_a2dp_codec_index_t A2DP_VendorSinkCodecIndexMihc(
    const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return BTAV_A2DP_CODEC_INDEX_SINK_MIHC;
}

btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexMihc(
    const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return BTAV_A2DP_CODEC_INDEX_SOURCE_MIHC;
}

const char* A2DP_VendorCodecNameMihc(const uint8_t* p_codec_info) {
  UNUSED(p_codec_info);
  return "MIHC";
}

bool A2DP_VendorCodecTypeEqualsMihc(const uint8_t* p_codec_info_a,
                                    const uint8_t* p_codec_info_b) {
  tA2DP_MIHC_CIE cie_a;
  tA2DP_MIHC_CIE cie_b;

  if (A2DP_ParseInfoMihc(&cie_a, p_codec_info_a, true) != A2D_SUCCESS)
    return false;
  if (A2DP_ParseInfoMihc(&cie_b, p_codec_info_b, true) != A2D_SUCCESS)
    return false;

  return true;
}

bool A2DP_VendorInitCodecConfigMihc(btav_a2dp_codec_index_t codec_index,
                                    uint8_t* p_result) {
  if (codec_index != BTAV_A2DP_CODEC_INDEX_SINK_MIHC) {
    return false;
  }
  return A2DP_BuildInfoMihc(A2D_MEDIA_TYPE_AUDIO, &a2dp_mihc_sink_caps,
                            p_result) == A2D_SUCCESS;
}

bool A2DP_VendorBuildCodecConfigMihc(uint8_t* p_src_cap, uint8_t* p_result) {
  tA2DP_MIHC_CIE src_cap;

  if (A2DP_ParseInfoMihc(&src_cap, p_src_cap, true) != A2D_SUCCESS) {
    return false;
  }

  /* MIHC negotiation is deliberately ENABLED so the phone will actually
   * negotiate MIHC and stream real MIHC frames at us.
   *
   * The purpose is bring-up: MIHC_FRAMEDUMP in a2dp_vendor_mihc_decoder.c hex-
   * dumps the raw over-the-air payloads so the MIHC-vs-LHDC framing difference
   * can be read directly off real frames (the BES MLAC implementation is dead
   * code in the firmware and carries no sync word of its own - the phone
   * negotiates by codecType==16 and a normal BluetoothCodecConfig).
   *
   * NOTE: the audio path is still the LHDC V5 engine, which may emit noise for
   * MIHC payloads. That is acceptable during bring-up: the goal is to capture
   * frames, not yet to play audio.
   *
   * To restore the previous "decline MIHC" behaviour, replace this function
   * body with `(void)src_cap; (void)p_result; return false;`.
   */

  /* Pick the highest mutually supported sample rate, preferring 48 kHz (what
   * the I2S output runs at) and falling back down the advertised list. */
  tA2DP_MIHC_CIE pref_cap = a2dp_mihc_default_config;
  if (src_cap.sampleRate & A2DP_MIHC_SAMPLING_FREQ_48000) {
    pref_cap.sampleRate = A2DP_MIHC_SAMPLING_FREQ_48000;
  } else if (src_cap.sampleRate & A2DP_MIHC_SAMPLING_FREQ_44100) {
    pref_cap.sampleRate = A2DP_MIHC_SAMPLING_FREQ_44100;
  } else if (src_cap.sampleRate & A2DP_MIHC_SAMPLING_FREQ_32000) {
    pref_cap.sampleRate = A2DP_MIHC_SAMPLING_FREQ_32000;
  } else if (src_cap.sampleRate & A2DP_MIHC_SAMPLING_FREQ_24000) {
    pref_cap.sampleRate = A2DP_MIHC_SAMPLING_FREQ_24000;
  } else if (src_cap.sampleRate & A2DP_MIHC_SAMPLING_FREQ_16000) {
    pref_cap.sampleRate = A2DP_MIHC_SAMPLING_FREQ_16000;
  } else {
    /* Peer offers only rates we do not implement; fail the config so the phone
     * falls back to another codec instead of starting a stream we cannot
     * describe. */
    return false;
  }

  if (src_cap.bitsPerSample & A2DP_MIHC_BITS_PER_SAMPLE_16) {
    pref_cap.bitsPerSample = A2DP_MIHC_BITS_PER_SAMPLE_16;
  } else if (src_cap.bitsPerSample & A2DP_MIHC_BITS_PER_SAMPLE_24) {
    pref_cap.bitsPerSample = A2DP_MIHC_BITS_PER_SAMPLE_24;
  }

  if (src_cap.channelMode & A2DP_MIHC_CHANNEL_MODE_STEREO) {
    pref_cap.channelMode = A2DP_MIHC_CHANNEL_MODE_STEREO;
  } else if (src_cap.channelMode & A2DP_MIHC_CHANNEL_MODE_MONO) {
    pref_cap.channelMode = A2DP_MIHC_CHANNEL_MODE_MONO;
  }

  /* Echo the peer's adaptive level so the SET_CONFIGURATION response matches
   * what the phone requested; a mismatch makes some stacks refuse to start. */
  pref_cap.chAdaptiveLevel = src_cap.chAdaptiveLevel;
  pref_cap.version = src_cap.version ? src_cap.version : A2DP_MIHC_VER_1;

  return A2DP_BuildInfoMihc(A2D_MEDIA_TYPE_AUDIO, &pref_cap, p_result) ==
         A2D_SUCCESS;
}

/* The decoder vtable. decoder_reset / decoder_start / decoder_suspend stay NULL
 * because the LHDC engine needs no explicit lifecycle handling there — the same
 * shape as a2dp_decoder_interface_lhdcv5. */
static const tA2DP_DECODER_INTERFACE a2dp_decoder_interface_mihc = {
    a2dp_mihc_decoder_init,
    a2dp_mihc_decoder_cleanup,
    NULL,
    a2dp_mihc_decoder_decode_packet_header,
    a2dp_mihc_decoder_decode_packet,
    NULL,
    NULL,
    a2dp_mihc_decoder_configure,
};

const tA2DP_DECODER_INTERFACE* A2DP_GetVendorDecoderInterfaceMihc(
    const uint8_t* p_codec_info) {
  if (!A2DP_IsVendorPeerSinkCodecValidMihc(p_codec_info)) return NULL;
  return &a2dp_decoder_interface_mihc;
}

#endif /* defined(MIHC_DEC_INCLUDED) && MIHC_DEC_INCLUDED == TRUE) */
