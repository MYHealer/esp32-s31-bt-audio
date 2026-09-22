#ifndef A2DP_VENDOR_MIHC_H
#define A2DP_VENDOR_MIHC_H

#include <stdbool.h>
#include <stdint.h>

#include "a2dp_codec_api.h"
#include "a2dp_vendor_mihc_constants.h"
#include "bt_av.h"
#include "stack/a2d_api.h"

/* The stock BES firmware keeps an array of 13 capability elements
 * (a2dp_mihc_avdtpcodec.elements[0..12]) and logs them individually, so the
 * fields below mirror what A2DP_BuildInfoMihc() serializes. Bit depth and the
 * channel-adaptive bitmap live in the trailer rather than in fixed byte slots,
 * hence the explicit has* flags. */
typedef struct {
  uint32_t vendorId;
  uint16_t codecId;
  uint8_t sampleRate;
  uint8_t bitsPerSample;
  uint8_t channelMode;
  uint8_t chAdaptiveLevel;
  uint8_t version;
} tA2DP_MIHC_CIE;

#ifdef __cplusplus
extern "C" {
#endif

tA2D_STATUS A2DP_BuildInfoMihc(uint8_t media_type,
                               const tA2DP_MIHC_CIE* p_ie,
                               uint8_t* p_result);
tA2D_STATUS A2DP_ParseInfoMihc(tA2DP_MIHC_CIE* p_ie,
                               const uint8_t* p_codec_info,
                               bool is_capability);
tA2D_STATUS A2DP_IsVendorPeerSourceCodecValidMihc(const uint8_t* p_codec_info);
bool A2DP_IsVendorPeerSinkCodecValidMihc(const uint8_t* p_codec_info);
btav_a2dp_codec_index_t A2DP_VendorSinkCodecIndexMihc(const uint8_t* p_codec_info);
btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexMihc(const uint8_t* p_codec_info);
const char* A2DP_VendorCodecNameMihc(const uint8_t* p_codec_info);
bool A2DP_VendorCodecTypeEqualsMihc(const uint8_t* p_codec_info_a,
                                    const uint8_t* p_codec_info_b);
bool A2DP_VendorInitCodecConfigMihc(btav_a2dp_codec_index_t codec_index,
                                    uint8_t* p_result);
bool A2DP_VendorBuildCodecConfigMihc(uint8_t* p_src_cap, uint8_t* p_result);

/* Returns the decoder vtable, or NULL if p_codec_info is not a valid MIHC sink
 * configuration. The audio path reuses the LHDC V5 decoder engine; see
 * a2dp_vendor_mihc_decoder.c. */
const tA2DP_DECODER_INTERFACE* A2DP_GetVendorDecoderInterfaceMihc(
    const uint8_t* p_codec_info);

#ifdef __cplusplus
}
#endif

#endif  // A2DP_VENDOR_MIHC_H
