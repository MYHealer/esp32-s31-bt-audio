/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define CODEC_ADC_SAMPLE_RATE      (48000)
#define CODEC_ADC_BITS_PER_SAMPLE  (16)
/* This board uses ES8389 STD I2S stereo (2ch), not Korvo-1 TDM 4ch. */
#define CODEC_ADC_CHANNELS         (2)
#define CODEC_DAC_SAMPLE_RATE      (48000)
#define CODEC_DAC_BITS_PER_SAMPLE  (16)
#define CODEC_DAC_CHANNELS         (2)

#ifdef __cplusplus
}
#endif  /* __cplusplus */
