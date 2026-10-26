/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_le.h"
#include "esp_bt_audio_vol.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_playback.h"
#include "esp_gmf_oal_sys.h"
#include "esp_bt_audio_tel.h"
#include "esp_bt_audio_pb.h"

#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "dev_audio_codec.h"
#include "cmd_reg.h"
#include "mihc_ota_payloads.h"
#include "mihc_one_live.h"
#include "esp_board_manager.h"
#include <math.h>
#include "esp_codec_dev.h"
#include "dev_audio_codec.h"
#include "driver/uart.h"

static uint8_t target_device_bda[6] = {0};
#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
static char target_device_name[32] = {0};
static const char *TAG = "CMD_REG";
#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */

static int cmd_playback_play(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_play();
    if (ret == ESP_OK) {
        printf("Media play command sent\n");
    } else {
        printf("Failed to send media play command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_pause(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_pause();
    if (ret == ESP_OK) {
        printf("Media pause command sent\n");
    } else {
        printf("Failed to send media pause command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_stop(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_stop();
    if (ret == ESP_OK) {
        printf("Media stop command sent\n");
    } else {
        printf("Failed to send media stop command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_next(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_next();
    if (ret == ESP_OK) {
        printf("Media next command sent\n");
    } else {
        printf("Failed to send media next command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_prev(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_prev();
    if (ret == ESP_OK) {
        printf("Media previous command sent\n");
    } else {
        printf("Failed to send media previous command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_metadata(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: metadata <mask>\n");
        printf("  mask bits: 0x1 title, 0x2 artist, 0x4 album, 0x8 track_num,\n");
        printf("             0x10 num_tracks, 0x20 genre, 0x40 time, 0x80 cover\n");
        return 1;
    }

    char *endptr = NULL;
    uint32_t mask = (uint32_t)strtoul(argv[1], &endptr, 0);
    if (argv[1][0] == '\0' || (endptr && *endptr != '\0')) {
        printf("Invalid mask: %s\n", argv[1]);
        return 1;
    }

    esp_err_t ret = esp_bt_audio_playback_request_metadata(mask);
    if (ret == ESP_OK) {
        printf("Media metadata request sent\n");
    } else {
        printf("Failed to send media metadata request: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_volume_set(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: vol_set <volume>\n");
        printf("  volume: 0-100\n");
        return 1;
    }

    int volume = atoi(argv[1]);
    if (volume < 0 || volume > 100) {
        printf("Volume must be between 0 and 100\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_absolute((uint32_t)volume);
    if (ret == ESP_OK) {
        printf("Volume set to %d\n", volume);
    } else {
        printf("Failed to set volume: %s\n", esp_err_to_name(ret));
    }
#else   /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    dev_audio_codec_handles_t *codec_handle = NULL;
    esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&codec_handle);
    esp_codec_dev_set_out_vol(codec_handle->codec_dev, volume);

    esp_err_t ret = esp_bt_audio_vol_notify((uint32_t)volume);
    if (ret == ESP_OK) {
        printf("Volume set to %d\n", volume);
    } else {
        printf("Failed to set volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

static int cmd_volume_up(int argc, char **argv)
{
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_relative(true);
    if (ret == ESP_OK) {
        printf("Volume up\n");
    } else {
        printf("Failed to increase volume: %s\n", esp_err_to_name(ret));
    }
#else   /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    int current_volume = 0;
    dev_audio_codec_handles_t *codec_handle = NULL;
    esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&codec_handle);
    esp_codec_dev_get_out_vol(codec_handle->codec_dev, &current_volume);
    current_volume = (current_volume >= 90) ? 100 : current_volume + 10;
    esp_codec_dev_set_out_vol(codec_handle->codec_dev, current_volume);

    esp_err_t ret = esp_bt_audio_vol_notify(current_volume);
    if (ret == ESP_OK) {
        printf("Volume up to %d\n", (int)current_volume);
    } else {
        printf("Failed to increase volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

static int cmd_volume_down(int argc, char **argv)
{
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_relative(false);
    if (ret == ESP_OK) {
        printf("Volume down\n");
    } else {
        printf("Failed to decrease volume: %s\n", esp_err_to_name(ret));
    }
#else   /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    int current_volume = 0;
    dev_audio_codec_handles_t *codec_handle = NULL;
    esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&codec_handle);
    esp_codec_dev_get_out_vol(codec_handle->codec_dev, &current_volume);
    current_volume = (current_volume <= 10) ? 0 : current_volume - 10;
    esp_codec_dev_set_out_vol(codec_handle->codec_dev, current_volume);

    esp_err_t ret = esp_bt_audio_vol_notify(current_volume);
    if (ret == ESP_OK) {
        printf("Volume down to %d\n", (int)current_volume);
    } else {
        printf("Failed to decrease volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
static bool is_target_bda_empty(void)
{
    for (int i = 0; i < sizeof(target_device_bda); ++i) {
        if (target_device_bda[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool parse_bda(const char *str, uint8_t *bda)
{
    return sscanf(str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                  &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) == 6;
}

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC && (CONFIG_GMF_EXAMPLE_A2DP_SOURCE || CONFIG_GMF_EXAMPLE_A2DP_SINK)
static int cmd_connect_device(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: connect <mac_address>\n");
        printf("Example: connect 01:02:03:04:05:06\n");
        return 1;
    }

    uint8_t bda[6] = {0};
    if (!parse_bda(argv[1], bda)) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, bda);
#elif CONFIG_GMF_EXAMPLE_A2DP_SINK
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, bda);
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    if (ret == ESP_OK) {
        printf("Connecting to device %s...\n", argv[1]);
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        printf("Failed to connect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_disconnect_device(int argc, char **argv)
{
    if (is_target_bda_empty()) {
        printf("No device is currently connected.\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, target_device_bda);
#elif CONFIG_GMF_EXAMPLE_A2DP_SINK
    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, target_device_bda);
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    if (ret == ESP_OK) {
        printf("Disconnecting device...\n");
        memset(target_device_bda, 0, sizeof(target_device_bda));
    } else {
        printf("Failed to disconnect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC && (CONFIG_GMF_EXAMPLE_A2DP_SOURCE || CONFIG_GMF_EXAMPLE_A2DP_SINK) */

#if CONFIG_GMF_EXAMPLE_HFP_HF
static int cmd_hf_connect(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: hf_connect <mac_address>\n");
        printf("Example: hf_connect 01:02:03:04:05:06\n");
        return 1;
    }

    uint8_t bda[6];
    if (!parse_bda(argv[1], bda)) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_HF, bda);
    if (ret == ESP_OK) {
        printf("Connecting HFP HF to device %s...\n", argv[1]);
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        printf("Failed to connect HFP HF: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_hf_disconnect(int argc, char **argv)
{
    if (is_target_bda_empty()) {
        printf("No device is currently connected.\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_HF, target_device_bda);
    if (ret == ESP_OK) {
        printf("Disconnecting HFP HF device...\n");
        memset(target_device_bda, 0, sizeof(target_device_bda));
    } else {
        printf("Failed to disconnect HFP HF: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_GMF_EXAMPLE_HFP_HF */

#if CONFIG_GMF_EXAMPLE_HFP_AG
static int cmd_ag_connect(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: ag_connect <mac_address>\n");
        printf("Example: ag_connect 01:02:03:04:05:06\n");
        return 1;
    }

    uint8_t bda[6] = {0};
    if (!parse_bda(argv[1], bda)) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }

    esp_bt_audio_classic_discovery_stop();

    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_AG, bda);
    if (ret == ESP_OK) {
        printf("Connecting HFP AG to device %s...\n", argv[1]);
        printf("Wait for log: Connection state: SLC_CONNECTED\n");
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        printf("Failed to connect HFP AG: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_ag_disconnect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (is_target_bda_empty()) {
        printf("No device is currently connected.\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_AG, target_device_bda);
    if (ret == ESP_OK) {
        printf("Disconnecting HFP AG device...\n");
        memset(target_device_bda, 0, sizeof(target_device_bda));
    } else {
        printf("Failed to disconnect HFP AG: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_GMF_EXAMPLE_HFP_AG */
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */

static int cmd_call_answer(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_call_answer(0);
    if (ret == ESP_OK) {
        printf("Call answer command sent\n");
    } else {
        printf("Failed to send call answer command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_call_reject(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_call_reject(0);
    if (ret == ESP_OK) {
        printf("Call reject/terminate command sent\n");
    } else {
        printf("Failed to send call reject/terminate command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_call_dial(int argc, char **argv)
{
    if (argc > 2) {
        printf("Usage: call_dial [number]\n");
        printf("  number: Optional. If omitted, use the role default number/redial behavior\n");
        return 1;
    }
    const char *number = (argc == 2) ? argv[1] : NULL;
    esp_err_t ret = esp_bt_audio_call_dial(number);
    if (ret == ESP_OK) {
        printf("Call dial command sent\n");
    } else {
        printf("Failed to send call dial command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_pb_fetch(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: pb_fetch <target> [start_idx] [count]\n");
        printf("  target (1-5 only):\n");
        printf("    1  Main phonebook\n");
        printf("    2  Incoming call history\n");
        printf("    3  Outgoing call history\n");
        printf("    4  Missed call history\n");
        printf("    5  Combined call history\n");
        printf("  start_idx: Start index (default 0)\n");
        printf("  count:     Number of entries, 0 = fetch all (default 0)\n");
        return 1;
    }

    char *endptr = NULL;
    unsigned long tv = strtoul(argv[1], &endptr, 0);
    if (argv[1][0] == '\0' || (endptr && *endptr != '\0') || tv < 1 || tv > 5) {
        printf("Invalid target: %s (use 1-5, see pb_fetch with no args)\n", argv[1]);
        return 1;
    }
    uint8_t target = (uint8_t)tv;

    uint16_t start_idx = 0;
    uint16_t count = 0;
    if (argc >= 3) {
        char *endptr = NULL;
        unsigned long v = strtoul(argv[2], &endptr, 0);
        if (argv[2][0] == '\0' || (endptr && *endptr != '\0') || v > 0xFFFF) {
            printf("Invalid start_idx: %s\n", argv[2]);
            return 1;
        }
        start_idx = (uint16_t)v;
    }
    if (argc >= 4) {
        char *endptr = NULL;
        unsigned long v = strtoul(argv[3], &endptr, 0);
        if (argv[3][0] == '\0' || (endptr && *endptr != '\0') || v > 0xFFFF) {
            printf("Invalid count: %s\n", argv[3]);
            return 1;
        }
        count = (uint16_t)v;
    }

    esp_err_t ret = esp_bt_audio_pb_fetch(target, start_idx, count);
    if (ret == ESP_OK) {
        printf("Phonebook/call history fetch request sent (target=%u, start=%u, count=%u)\n",
               (unsigned)target, (unsigned)start_idx, (unsigned)count);
    } else {
        printf("Failed to send pb_fetch: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
static int cmd_le_scan_start(int argc, char **argv)
{
    uint32_t timeout_ms = CONFIG_GMF_EXAMPLE_LE_SCAN_TIMEOUT_MS;

    if (argc >= 2) {
        timeout_ms = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    esp_err_t ret = esp_bt_audio_le_scan_start(timeout_ms);
    if (ret == ESP_OK) {
        printf("LE scan started (%u ms)\n", (unsigned)timeout_ms);
    } else {
        printf("Failed to start LE scan: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_scan_stop(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_le_scan_stop();
    if (ret == ESP_OK) {
        printf("LE scan stopped\n");
    } else {
        printf("Failed to stop LE scan: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_connect(int argc, char **argv)
{
    uint8_t bda[6] = {0};
    uint8_t addr_type = 0;
    uint32_t timeout_ms = CONFIG_GMF_EXAMPLE_LE_SCAN_TIMEOUT_MS;

    if (argc < 3) {
        printf("Usage: le_connect <addr_type> <mac_address> [timeout_ms]\n");
        printf("Example: le_connect 0 01:02:03:04:05:06 10000\n");
        return 1;
    }
    addr_type = (uint8_t)strtoul(argv[1], NULL, 0);
    if (sscanf(argv[2], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) != 6) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }
    if (argc >= 4) {
        timeout_ms = (uint32_t)strtoul(argv[3], NULL, 0);
    }
    esp_err_t ret = esp_bt_audio_le_connect(addr_type, bda, timeout_ms);
    if (ret == ESP_OK) {
        printf("LE connect started (%s)\n", argv[2]);
    } else {
        printf("Failed to start LE connect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_disconnect(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_le_disconnect();
    if (ret == ESP_OK) {
        printf("LE disconnect requested\n");
    } else {
        printf("Failed to disconnect LE link: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

#ifdef CONFIG_GMF_EXAMPLE_LE_TMAP_ROLE_BMS
static int cmd_bms_start(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_le_broadcast_source_start();
    if (ret == ESP_OK) {
        printf("BMS stream started\n");
    } else {
        printf("Failed to start BMS stream: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_bms_stop(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_le_broadcast_source_stop();
    if (ret == ESP_OK) {
        printf("BMS stream stopped\n");
    } else {
        printf("Failed to stop BMS stream: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_GMF_EXAMPLE_LE_TMAP_ROLE_BMS */
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */

#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
static int cmd_start_discovery(int argc, char **argv)
{
    memset(target_device_name, 0, sizeof(target_device_name));

    if (argc == 2) {
        strncpy(target_device_name, argv[1], sizeof(target_device_name) - 1);
        target_device_name[sizeof(target_device_name) - 1] = '\0';
        printf("Discovery started - looking for device: %s\n", target_device_name);
    } else if (argc == 1) {
        printf("Discovery started - scanning for all devices\n");
    } else {
        printf("Usage: start_discovery [device_name]\n");
        printf("  device_name: Optional. If provided, will auto-connect to this device\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_discovery_start();
    if (ret == ESP_OK) {
        if (strlen(target_device_name) > 0) {
            printf("Will auto-connect to '%s' when found\n", target_device_name);
        }
    } else {
        printf("Failed to start discovery: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_stop_discovery(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_classic_discovery_stop();
    if (ret == ESP_OK) {
        printf("Discovery stopped\n");
    } else {
        printf("Failed to stop discovery: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_start_media(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (ret == ESP_OK) {
        printf("Media started\n");
    } else {
        printf("Failed to start media: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_stop_media(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    if (ret == ESP_OK) {
        printf("Media stopped\n");
    } else {
        printf("Failed to stop media: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */

void cli_register_bt(void)
{
    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "Send media play command",
        .hint = NULL,
        .func = &cmd_playback_play,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));

    const esp_console_cmd_t pause_cmd = {
        .command = "pause",
        .help = "Send media pause command",
        .hint = NULL,
        .func = &cmd_playback_pause,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pause_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "Send media stop command",
        .hint = NULL,
        .func = &cmd_playback_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t next_cmd = {
        .command = "next",
        .help = "Send media next track command",
        .hint = NULL,
        .func = &cmd_playback_next,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&next_cmd));

    const esp_console_cmd_t prev_cmd = {
        .command = "prev",
        .help = "Send media previous track command",
        .hint = NULL,
        .func = &cmd_playback_prev,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prev_cmd));

    const esp_console_cmd_t metadata_cmd = {
        .command = "metadata",
        .help = "Request media metadata with mask",
        .hint = "<mask>",
        .func = &cmd_playback_metadata,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&metadata_cmd));

    const esp_console_cmd_t vol_set_cmd = {
        .command = "vol_set",
        .help = "Set volume level (0-100)",
        .hint = "<volume>",
        .func = &cmd_volume_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_set_cmd));

    const esp_console_cmd_t vol_up_cmd = {
        .command = "vol_up",
        .help = "Increase volume by 10",
        .hint = NULL,
        .func = &cmd_volume_up,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_up_cmd));

    const esp_console_cmd_t vol_down_cmd = {
        .command = "vol_down",
        .help = "Decrease volume by 10",
        .hint = NULL,
        .func = &cmd_volume_down,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_down_cmd));

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC && (CONFIG_GMF_EXAMPLE_A2DP_SOURCE || CONFIG_GMF_EXAMPLE_A2DP_SINK)
    {
        const esp_console_cmd_t connect_cmd = {
            .command = "connect",
            .help = "Connect to a Bluetooth device",
            .hint = "<mac_address>",
            .func = &cmd_connect_device,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&connect_cmd));
    }

    {
        const esp_console_cmd_t disconnect_cmd = {
            .command = "disconnect",
            .help = "Disconnect from current Bluetooth device",
            .hint = NULL,
            .func = &cmd_disconnect_device,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&disconnect_cmd));
    }

#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC && (CONFIG_GMF_EXAMPLE_A2DP_SOURCE || CONFIG_GMF_EXAMPLE_A2DP_SINK) */

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
#if CONFIG_GMF_EXAMPLE_HFP_HF
    {
        const esp_console_cmd_t hf_connect_cmd = {
            .command = "hf_connect",
            .help = "Connect HFP HF to a Bluetooth device",
            .hint = "<mac_address>",
            .func = &cmd_hf_connect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&hf_connect_cmd));
    }

    {
        const esp_console_cmd_t hf_disconnect_cmd = {
            .command = "hf_disconnect",
            .help = "Disconnect HFP HF from current Bluetooth device",
            .hint = NULL,
            .func = &cmd_hf_disconnect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&hf_disconnect_cmd));
    }
#endif  /* CONFIG_GMF_EXAMPLE_HFP_HF */

#if CONFIG_GMF_EXAMPLE_HFP_AG
    {
        const esp_console_cmd_t ag_connect_cmd = {
            .command = "ag_connect",
            .help = "Connect HFP AG to a Bluetooth HF device",
            .hint = "<mac_address>",
            .func = &cmd_ag_connect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ag_connect_cmd));
    }

    {
        const esp_console_cmd_t ag_disconnect_cmd = {
            .command = "ag_disconnect",
            .help = "Disconnect HFP AG from current Bluetooth device",
            .hint = NULL,
            .func = &cmd_ag_disconnect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ag_disconnect_cmd));
    }

#endif  /* CONFIG_GMF_EXAMPLE_HFP_AG */
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */

    const esp_console_cmd_t call_answer_cmd = {
        .command = "call_answer",
        .help = "Answer incoming call or simulate AG answer",
        .hint = NULL,
        .func = &cmd_call_answer,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_answer_cmd));

    const esp_console_cmd_t call_reject_cmd = {
        .command = "call_reject",
        .help = "Reject or terminate call",
        .hint = NULL,
        .func = &cmd_call_reject,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_reject_cmd));

    const esp_console_cmd_t call_dial_cmd = {
        .command = "call_dial",
        .help = "Dial number or simulate AG outgoing call",
        .hint = "[number]",
        .func = &cmd_call_dial,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_dial_cmd));

    const esp_console_cmd_t pb_fetch_cmd = {
        .command = "pb_fetch",
        .help = "Fetch phonebook or call history, target 1-5 = main_pb/incoming/outgoing/missed/combined",
        .hint = "<target> [start_idx] [count]",
        .func = &cmd_pb_fetch,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pb_fetch_cmd));

#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
    const esp_console_cmd_t start_discovery_cmd = {
        .command = "start_discovery",
        .help = "Start Bluetooth device discovery with optional auto-connect",
        .hint = "[device_name]",
        .func = &cmd_start_discovery,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_discovery_cmd));

    const esp_console_cmd_t stop_discovery_cmd = {
        .command = "stop_discovery",
        .help = "Stop Bluetooth device discovery",
        .hint = NULL,
        .func = &cmd_stop_discovery,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_discovery_cmd));

    const esp_console_cmd_t start_media_cmd = {
        .command = "start_media",
        .help = "Start media playback",
        .hint = NULL,
        .func = &cmd_start_media,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_media_cmd));

    const esp_console_cmd_t stop_media_cmd = {
        .command = "stop_media",
        .help = "Stop media playback",
        .hint = NULL,
        .func = &cmd_stop_media,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_media_cmd));
#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */

#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
    {
        const esp_console_cmd_t le_scan_start_cmd = {
            .command = "le_scan_start",
            .help = "Start LE scan",
            .hint = "[timeout_ms]",
            .func = &cmd_le_scan_start,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_scan_start_cmd));
    }

    {
        const esp_console_cmd_t le_scan_stop_cmd = {
            .command = "le_scan_stop",
            .help = "Stop LE scan",
            .hint = NULL,
            .func = &cmd_le_scan_stop,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_scan_stop_cmd));
    }

    {
        const esp_console_cmd_t le_connect_cmd = {
            .command = "le_connect",
            .help = "Connect LE peer",
            .hint = "<addr_type> <mac_address> [timeout_ms]",
            .func = &cmd_le_connect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_connect_cmd));
    }

    {
        const esp_console_cmd_t le_disconnect_cmd = {
            .command = "le_disconnect",
            .help = "Disconnect LE ACL link",
            .hint = NULL,
            .func = &cmd_le_disconnect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_disconnect_cmd));
    }

#ifdef CONFIG_GMF_EXAMPLE_LE_TMAP_ROLE_BMS
    {
        const esp_console_cmd_t bms_start_cmd = {
            .command = "bms_start",
            .help = "Start BMS stream",
            .hint = NULL,
            .func = &cmd_bms_start,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&bms_start_cmd));
    }

    {
        const esp_console_cmd_t bms_stop_cmd = {
            .command = "bms_stop",
            .help = "Stop BMS stream",
            .hint = NULL,
            .func = &cmd_bms_stop,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&bms_stop_cmd));
    }
#endif  /* CONFIG_GMF_EXAMPLE_LE_TMAP_ROLE_BMS */
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */
}


/* --- MIHC serial backdoor: decode quality + heap + stack watermarks --- */
extern volatile int32_t g_mihc_tap_ready, g_mihc_tap_peak, g_mihc_tap_nz;
extern volatile int32_t g_mihc_tap_ac1_q15, g_mihc_tap_zcr_q15, g_mihc_tap_frames;
extern volatile int16_t g_mihc_tap_pcm[32];
/* DOWNSTREAM tap: bytes actually handed to the GMF/ASRC path */
extern volatile int32_t g_mihc_emit_chunks, g_mihc_emit_len, g_mihc_emit_ac1_q15;
extern volatile int32_t g_mihc_emit_zcr_q15, g_mihc_emit_peak, g_mihc_emit_head;
extern volatile int32_t g_mihc_emit_frame_of[4];
extern volatile int16_t g_mihc_emit_pcm[4][8];

/* --- DAC-exit tap: last bytes handed to esp_codec_dev_write -> I2S --- */
extern volatile int32_t g_dac_tap_chunks, g_dac_tap_len, g_dac_tap_ac1_q15;
extern volatile int32_t g_dac_tap_zcr_q15, g_dac_tap_peak;
extern volatile int16_t g_dac_tap_pcm[16];

static int cmd_mihc_dacstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    int ac1 = (int)g_dac_tap_ac1_q15, zcr = (int)g_dac_tap_zcr_q15;
    printf("DAC_TAP chunks=%d len=%d peak=%d ac1_q15=%d zcr_q15=%d\n",
           (int)g_dac_tap_chunks, (int)g_dac_tap_len, (int)g_dac_tap_peak, ac1, zcr);
    if (g_dac_tap_chunks == 0)            printf("DAC_VERDICT: NO_WRITE\n");
    else if (g_dac_tap_peak < 32)         printf("DAC_VERDICT: SILENCE\n");
    else if (ac1 > 20000 && zcr < 8000)   printf("DAC_VERDICT: MUSIC_LIKE\n");
    else if (ac1 < 4000 && zcr > 12000)   printf("DAC_VERDICT: WHITE_NOISE\n");
    else                                  printf("DAC_VERDICT: UNCERTAIN\n");
    printf("DAC_PCM[0..15]:");
    for (int i = 0; i < 16; i++) printf(" %d", (int)g_dac_tap_pcm[i]);
    printf("\n");
    return 0;
}

/* --- ASRC info tap: upstream (aud_dec) reported info vs ASRC src/dest --- */
extern volatile int32_t g_asrc_up_rate, g_asrc_up_ch, g_asrc_up_bits;
extern volatile int32_t g_asrc_src_rate, g_asrc_src_ch, g_asrc_src_bits;
extern volatile int32_t g_asrc_dst_rate, g_asrc_dst_ch, g_asrc_dst_bits;
extern volatile int32_t g_asrc_need_reopen, g_asrc_bypass, g_asrc_updates;

static int cmd_mihc_asrcstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("ASRC updates=%d\n", (int)g_asrc_updates);
    printf("ASRC UPSTREAM(aud_dec): rate=%d ch=%d bits=%d\n",
           (int)g_asrc_up_rate, (int)g_asrc_up_ch, (int)g_asrc_up_bits);
    printf("ASRC SRC: rate=%d ch=%d bits=%d\n",
           (int)g_asrc_src_rate, (int)g_asrc_src_ch, (int)g_asrc_src_bits);
    printf("ASRC DST: rate=%d ch=%d bits=%d\n",
           (int)g_asrc_dst_rate, (int)g_asrc_dst_ch, (int)g_asrc_dst_bits);
    printf("ASRC need_reopen=%d bypass=%d\n",
           (int)g_asrc_need_reopen, (int)g_asrc_bypass);
    if (g_asrc_updates == 0) {
        printf("ASRC_VERDICT: NO_UPDATE (no stream info received)\n");
    } else if (g_asrc_bypass) {
        printf("ASRC_VERDICT: BYPASS_OK (src==dst, ASRC is transparent)\n");
    } else {
        printf("ASRC_VERDICT: ***** ACTIVE_CONVERSION (src!=dst -> ASRC mangles PCM) *****\n");
    }
    return 0;
}

/* --- RAW callback tap: what bt_a2d_sink_raw_data_cb actually received --- */
extern volatile int32_t g_raw_cb_count, g_raw_len, g_raw_ac1_q15, g_raw_peak;
extern volatile int16_t g_raw_pcm[16];

static int cmd_mihc_rawstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("RAW cb=%d len=%d peak=%d ac1_q15=%d\n",
           (int)g_raw_cb_count, (int)g_raw_len, (int)g_raw_peak, (int)g_raw_ac1_q15);
    printf("RAW_PCM[0..15]:");
    for (int i = 0; i < 16; i++) printf(" %d", (int)g_raw_pcm[i]);
    printf("\n");
    printf("  compare vs mihc_diag EMIT_RING[0] (kernel-side, same frame)\n");
    return 0;
}

/* --- queue tap: enqueue/drop counters on data_q (bt_a2d_sink_data_cb) --- */
extern volatile int32_t g_q_drop, g_q_sendfail, g_q_sendtotal, g_q_readyhigh;

static int cmd_mihc_queuestat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Q send=%d drop=%d sendfail=%d readyhigh=%d\n",
           (int)g_q_sendtotal, (int)g_q_drop, (int)g_q_sendfail, (int)g_q_readyhigh);
    if (g_q_sendtotal == 0) {
        printf("Q_VERDICT: NO_SEND (data_q never received a packet)\n");
    } else if (g_q_drop > 0 || g_q_sendfail > 0) {
        printf("Q_VERDICT: ***** DROPPING (%d drop + %d fail of %d) -> periodic discontinuity *****\n",
               (int)g_q_drop, (int)g_q_sendfail, (int)g_q_sendtotal);
    } else {
        printf("Q_VERDICT: NO_DROP (readyhigh=%d, %d packets all delivered)\n",
               (int)g_q_readyhigh, (int)g_q_sendtotal);
    }
    return 0;
}

static int cmd_mihc_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("MIHC_DIAG frames=%d ready=%d peak=%d nz=%d ac1_q15=%d zcr_q15=%d\n",
           (int)g_mihc_tap_frames, (int)g_mihc_tap_ready,
           (int)g_mihc_tap_peak, (int)g_mihc_tap_nz,
           (int)g_mihc_tap_ac1_q15, (int)g_mihc_tap_zcr_q15);
    int ac1 = (int)g_mihc_tap_ac1_q15, zcr = (int)g_mihc_tap_zcr_q15;
    if (!g_mihc_tap_ready) {
        printf("VERDICT: NO_DECODE\n");
    } else if (g_mihc_tap_peak < 32) {
        printf("VERDICT: SILENCE\n");
    } else if (ac1 > 20000 && zcr < 8000) {
        printf("VERDICT: MUSIC_LIKE\n");
    } else if (ac1 < 4000 && zcr > 12000) {
        printf("VERDICT: WHITE_NOISE\n");
    } else {
        printf("VERDICT: UNCERTAIN\n");
    }
    printf("PCM[0..15]:");
    for (int i = 0; i < 16; i++) {
        printf(" %d", (int)g_mihc_tap_pcm[i]);
    }
    printf("\n");
    /* DOWNSTREAM tap — must match the kernel tap above if same frame.
     * If these two disagree in the SAME diag invocation, the corruption is
     * between decode_packet() and the GMF admission, not in the kernel. */
    printf("EMIT chunks=%d len=%d ac1_q15=%d zcr_q15=%d peak=%d head=%d\n",
           (int)g_mihc_emit_chunks, (int)g_mihc_emit_len,
           (int)g_mihc_emit_ac1_q15, (int)g_mihc_emit_zcr_q15,
           (int)g_mihc_emit_peak, (int)g_mihc_emit_head);
    for (int s = 0; s < 4; s++) {
        int idx = ((g_mihc_emit_head - 1 - s) & 3);
        printf("EMIT_RING[%d] frame=%d:", s, (int)g_mihc_emit_frame_of[idx]);
        for (int i = 0; i < 8; i++) printf(" %d", (int)g_mihc_emit_pcm[idx][i]);
        printf("\n");
    }
    printf("heap int=%u psram=%u min_int=%u min_psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    TaskHandle_t h;
    const char *names[] = { "A2DP_DECODER", "LVGL", "main", "IDLE0", "IDLE1", NULL };
    for (int i = 0; names[i]; i++) {
        h = xTaskGetHandle(names[i]);
        if (h) {
            printf("stack_wm %s = %u\n", names[i],
                   (unsigned)uxTaskGetStackHighWaterMark(h));
        }
    }
    return 0;
}


/* Decode embedded real OTA payloads on-device (no phone needed). */
extern int32_t mihc_celt_decode_with_ec(void *st, const uint8_t *data, int32_t nb_bytes,
                                        int16_t *pcm_out, int32_t channels,
                                        void *r0, void *r1, void *ec);
extern void mihc_celt_reset(void);

static int cmd_mihc_selfplay(int argc, char **argv)
{
    (void)argc; (void)argv;
    static uint8_t st[4096];
    static int16_t pcm[2 * 480];
    mihc_celt_reset();
    int ok = 0;
    int64_t energy = 0;
    int peak_all = 0;
    for (int f = 0; f < MIHC_OTA_NFRAMES; f++) {
        memset(pcm, 0, sizeof(pcm));
        int32_t rc = mihc_celt_decode_with_ec(st, mihc_ota_payloads[f], MIHC_OTA_FRAME_LEN,
                                              pcm, 2, NULL, NULL, NULL);
        int peak = 0, nz = 0;
        for (int i = 0; i < 2 * 480; i++) {
            int v = pcm[i]; if (v < 0) v = -v;
            if (v > peak) peak = v;
            if (pcm[i]) nz++;
            energy += (int64_t)pcm[i] * pcm[i];
        }
        if (peak > peak_all) peak_all = peak;
        printf("selfplay f=%d rc=%d peak=%d nz=%d\n", f, (int)rc, peak, nz);
        if (nz > 0) ok++;
    }
    /* tap-like metrics from last frame */
    int ac1 = 0, zcr = 0;
    {
        int64_t a = 0, n = 0; int16_t prev = 0; int zc = 0;
        for (int i = 0; i < 480; i++) {
            int16_t v = pcm[2 * i];
            a += (int32_t)prev * v; n += (int32_t)prev * prev;
            if ((prev < 0) != (v < 0)) zc++;
            prev = v;
        }
        ac1 = n ? (int)((a << 15) / n) : 0;
        zcr = (int)((zc << 15) / 480);
    }
    printf("DEV16:");
    for (int i = 0; i < 16; i++) printf(" %d", pcm[i]);
    printf("\n");
    printf("selfplay SUMMARY ok=%d/%d energy=%lld peak=%d ac1_q15=%d zcr_q15=%d\n",
           ok, MIHC_OTA_NFRAMES, (long long)energy, peak_all, ac1, zcr);
    if (ok == 0) {
        printf("SELFPLAY_VERDICT: DECODE_FAIL\n");
    } else if (peak_all < 32) {
        printf("SELFPLAY_VERDICT: SILENCE\n");
    } else if (ac1 > 20000 && zcr < 8000) {
        printf("SELFPLAY_VERDICT: MUSIC_LIKE\n");
    } else if (ac1 < 4000 && zcr > 12000) {
        printf("SELFPLAY_VERDICT: WHITE_NOISE\n");
    } else {
        printf("SELFPLAY_VERDICT: UNCERTAIN\n");
    }
    return 0;
}


/* Write decoded PCM straight to DAC, bypassing GMF. If this sounds like
 * music, GMF is the noise source. If still white noise, I2S/DAC is. */
static int cmd_mihc_dac(int argc, char **argv)
{
    (void)argc; (void)argv;
    extern int32_t mihc_celt_decode_with_ec(void *st, const uint8_t *data, int32_t nb_bytes,
                                            int16_t *pcm_out, int32_t channels,
                                            void *r0, void *r1, void *ec);
    extern void mihc_celt_reset(void);
    dev_audio_codec_handles_t *h = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&h) != ESP_OK || !h) {
        printf("no DAC handle\n");
        return 1;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 48000, .bits_per_sample = 16, .channel = 2,
        .mclk_multiple = 256, .channel_mask = 0x03,
    };
    /* PA (GPIO47, active HIGH) is only enabled by A2DP stream start.
     * Direct DAC writes need it forced on or there is NO sound. */
    {
        extern esp_err_t gpio_config(void *);
        extern esp_err_t gpio_set_level(int, int);
        typedef struct { unsigned long pin_bit_mask; int mode; int pull_up_en; int pull_down_en; int intr_type; } fake_gpio_cfg_t;
        fake_gpio_cfg_t cfg = { .pin_bit_mask = 1UL << 47, .mode = 3, .pull_up_en = 0, .pull_down_en = 0, .intr_type = 0 };
        gpio_config(&cfg);
        gpio_set_level(47, 1);
        printf("PA ON gpio47\n");
    }
    int err = esp_codec_dev_open(h->codec_dev, &fs);
    printf("dac open %d\n", err);
    static uint8_t st[4096];
    static int16_t pcm[2 * 480];
    mihc_celt_reset();
    /* 5 loops of embedded frames = ~0.4s music */
    for (int loop = 0; loop < 20; loop++) {
        for (int f = 0; f < MIHC_OTA_NFRAMES; f++) {
            memset(pcm, 0, sizeof(pcm));
            mihc_celt_decode_with_ec(st, mihc_ota_payloads[f], MIHC_OTA_FRAME_LEN,
                                     pcm, 2, NULL, NULL, NULL);
            int w = esp_codec_dev_write(h->codec_dev, pcm, (int)sizeof(pcm));
            if (w != 0) { printf("write err %d\n", w); break; }
        }
    }
    printf("MIHC_DAC_DONE\n");
    return 0;
}

/* Print last N live OTA payloads (ring buffer) as hex for host replay. */
extern volatile int32_t g_mihc_dump_ready, g_mihc_dump_nf, g_mihc_dump_len[];
extern volatile uint8_t g_mihc_dump_pay[][620];
extern volatile int32_t g_mihc_dump_seq[], g_mihc_dump_head, g_mihc_dump_count;

static int cmd_mihc_dump(int argc, char **argv)
{
    (void)argc; (void)argv;
    int nf = g_mihc_dump_nf;
    int head = g_mihc_dump_head;
    printf("DUMP nf=%d head=%d total=%d\n", nf, head, (int)g_mihc_dump_count);
    if (nf <= 0) {
        printf("NO_FRAMES\n");
        return 0;
    }
    int start = head - nf;
    if (start < 0) start += 8;
    for (int i = 0; i < nf; i++) {
        int idx = (start + i) % 8;
        int n = (int)g_mihc_dump_len[idx];
        printf("PAYLOAD %d len=%d ", i, n);
        for (int j = 0; j < n && j < 620; j++) printf("%02x", g_mihc_dump_pay[idx][j]);
        printf("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* mihc_stream : continuously push NEW payloads (TOC+EC) to UART0 TX.  */
/*  Frame wire format (binary, no ASCII):                              */
/*     [0xA5][seq:2 LE][plen:2 LE][payload ...]                        */
/*  seq = g_mihc_dump_seq (monotonic per decoded frame). PC joins      */
/*  consecutive seq to rebuild the live stream; gaps => drop detected. */
/*  Runs resident in the REPL task with a generous time budget; Ctrl-C  */
/*  (or the iteration cap) ends it.                                    */
/* ------------------------------------------------------------------ */
#define MIHC_STREAM_RING 8      /* mirrors MIHC_DUMP_NF in the decoder */
#define MIHC_STREAM_MAX_ITER 6000    /* ~120 s at 20 ms/iter */
#define MIHC_STREAM_PERIOD_MS 20

static int cmd_mihc_stream(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Copy-once per frame: snapshot the slot BEFORE writing, so the payload
     * bytes we send belong to one seq slot even if the decoder overwrites
     * the slot mid-send (detected by PC as a seq gap, not a corrupt blob). */
    static uint8_t slot[620];
    int32_t last = g_mihc_dump_count;            /* only NEW frames */
    int64_t sent_frames = 0, sent_bytes = 0;
    printf("STREAM start last=%d (921600 baud)\n", (int)last);

    for (int iter = 0; iter < MIHC_STREAM_MAX_ITER; iter++) {
        int32_t now = g_mihc_dump_count;
        while (last < now) {
            uint8_t hdr[5];
            int32_t seq = last;
            int32_t idx = seq % MIHC_STREAM_RING;
            if (idx >= 0 && idx < MIHC_STREAM_RING) {
                int32_t n = g_mihc_dump_len[idx];
                if (n > 0 && n <= 620) {
                    memcpy(slot, g_mihc_dump_pay[idx], (size_t)n);
                    hdr[0] = 0xA5;
                    hdr[1] = (uint8_t)(seq & 0xFF);
                    hdr[2] = (uint8_t)((seq >> 8) & 0xFF);
                    hdr[3] = (uint8_t)(n & 0xFF);
                    hdr[4] = (uint8_t)((n >> 8) & 0xFF);
                    uart_write_bytes(UART_NUM_0, hdr, sizeof(hdr));
                    uart_write_bytes(UART_NUM_0, slot, (size_t)n);
                    sent_frames++;
                    sent_bytes += n;
                }
            }
            last = now;
        }
        if ((sent_frames % 500) == 0 && sent_frames > 0) {
            printf("\nSTREAM n=%d f=%lld B=%lld\n",
                   (int)now, (long long)sent_frames, (long long)sent_bytes);
        }
        vTaskDelay(pdMS_TO_TICKS(MIHC_STREAM_PERIOD_MS));
    }
    printf("\nSTREAM_END frames=%lld bytes=%lld\n",
           (long long)sent_frames, (long long)sent_bytes);
    return 0;
}

static int cmd_mihc_sine(int argc, char **argv)
{
    (void)argc; (void)argv;
    dev_audio_codec_handles_t *h = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&h) != ESP_OK || !h) {
        printf("no DAC\n");
        return 1;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 48000, .bits_per_sample = 16, .channel = 2,
        .mclk_multiple = 256, .channel_mask = 0x03,
    };
    /* PA (GPIO47, active HIGH) is only enabled by A2DP stream start.
     * Direct DAC writes need it forced on or there is NO sound. */
    {
        extern esp_err_t gpio_config(void *);
        extern esp_err_t gpio_set_level(int, int);
        typedef struct { unsigned long pin_bit_mask; int mode; int pull_up_en; int pull_down_en; int intr_type; } fake_gpio_cfg_t;
        fake_gpio_cfg_t cfg = { .pin_bit_mask = 1UL << 47, .mode = 3, .pull_up_en = 0, .pull_down_en = 0, .intr_type = 0 };
        gpio_config(&cfg);
        gpio_set_level(47, 1);
        printf("PA ON gpio47\n");
    }
    printf("sine open %d\n", esp_codec_dev_open(h->codec_dev, &fs));
    static int16_t pcm[2 * 480];
    float ph = 0.0f;
    for (int beep = 0; beep < 3; beep++) {
        printf(">>> BEEP %d/3\n", beep + 1);
        for (int blk = 0; blk < 50; blk++) {
            for (int i = 0; i < 480; i++) {
                int16_t s = (int16_t)(sinf(ph) * 12000.0f);
                pcm[2 * i] = s; pcm[2 * i + 1] = s;
                ph += 2.0f * 3.1415926f * 1000.0f / 48000.0f;
                if (ph > 6.2831853f) ph -= 6.2831853f;
            }
            esp_codec_dev_write(h->codec_dev, pcm, (int)sizeof(pcm));
        }
        printf("<<< BEEP %d/3 silence\n", beep + 1);
        memset(pcm, 0, sizeof(pcm));
        for (int blk = 0; blk < 50; blk++) esp_codec_dev_write(h->codec_dev, pcm, (int)sizeof(pcm));
    }
    printf("SINE_TEST_DONE\n");
    return 0;
}

static int cmd_mihc_one(int argc, char **argv)
{
    (void)argc; (void)argv;
    static uint8_t st[4096];
    static int16_t pcm[2 * 480];
    mihc_celt_reset();
    memset(pcm, 0, sizeof(pcm));
    int32_t rc = mihc_celt_decode_with_ec(st, mihc_one_live, MIHC_ONE_LIVE_LEN,
                                          pcm, 2, NULL, NULL, NULL);
    printf("ONE rc=%d len=%d\n", (int)rc, MIHC_ONE_LIVE_LEN);
    printf("DEV16:");
    for (int i = 0; i < 16; i++) printf(" %d", pcm[i]);
    printf("\n");
    int64_t a = 0, n = 0; int16_t prev = 0;
    for (int i = 0; i < 480; i++) {
        int16_t v = pcm[2 * i];
        a += (int32_t)prev * v;
        n += (int32_t)prev * prev;
        prev = v;
    }
    printf("ONE_SUMMARY ac1_q15=%d peak_check=%d\n",
           n ? (int)((a << 15) / n) : 0, pcm[0]);
    return 0;
}

static int restart(int argc, char **argv)
{
    printf("Restarting\n");
    esp_restart();
}

static int free_mem(int argc, char **argv)
{
    printf("\nFree heap size: internal %u, psram %u\nmin  heap size: internal %u, psram %u\n",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    return 0;
}

/** 'tasks' command prints the list of tasks and related information */
static int tasks_info(int argc, char **argv)
{
    return esp_gmf_oal_sys_get_real_time_stats(1000, false);
}

static bool parse_log_level(const char *level_str, esp_log_level_t *level)
{
    if (strcmp(level_str, "none") == 0 || strcmp(level_str, "0") == 0) {
        *level = ESP_LOG_NONE;
    } else if (strcmp(level_str, "error") == 0 || strcmp(level_str, "err") == 0 || strcmp(level_str, "1") == 0) {
        *level = ESP_LOG_ERROR;
    } else if (strcmp(level_str, "warn") == 0 || strcmp(level_str, "warning") == 0 || strcmp(level_str, "2") == 0) {
        *level = ESP_LOG_WARN;
    } else if (strcmp(level_str, "info") == 0 || strcmp(level_str, "3") == 0) {
        *level = ESP_LOG_INFO;
    } else if (strcmp(level_str, "debug") == 0 || strcmp(level_str, "4") == 0) {
        *level = ESP_LOG_DEBUG;
    } else if (strcmp(level_str, "verbose") == 0 || strcmp(level_str, "5") == 0) {
        *level = ESP_LOG_VERBOSE;
    } else {
        return false;
    }
    return true;
}

static int log_level(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: log_level <tag|*> <none|error|warn|info|debug|verbose|0-5>\n");
        printf("Example: log_level CLK_SYNC_EL debug\n");
        printf("Example: log_level * warn\n");
        return 1;
    }

    esp_log_level_t level = ESP_LOG_NONE;
    if (!parse_log_level(argv[2], &level)) {
        printf("Invalid log level: %s\n", argv[2]);
        return 1;
    }

    esp_log_level_set(argv[1], level);
    printf("Set log level: tag=%s level=%s\n", argv[1], argv[2]);
    return 0;
}

void cli_register_sys()
{
    static const esp_console_cmd_t cmds[] = {
        {
            .command = "restart",
            .help = "Software reset of the chip",
            .hint = NULL,
            .func = &restart,
        },
        {
            .command = "free",
            .help = "Get the current size of free heap memory",
            .hint = NULL,
            .func = &free_mem,
        },
        {
            .command = "tasks",
            .help = "Get information about running tasks",
            .hint = NULL,
            .func = &tasks_info,
        },
        {
            .command = "mihc_diag",
            .help = "MIHC decode quality + heap + stack watermarks",
            .hint = NULL,
            .func = &cmd_mihc_diag,
        },
        {
            .command = "mihc_dacstat",
            .help = "DAC-exit tap: PCM actually written to I2S (post-GMF/ASRC)",
            .hint = NULL,
            .func = &cmd_mihc_dacstat,
        },
        {
            .command = "mihc_asrcstat",
            .help = "ASRC tap: upstream info vs ASRC src/dest, bypass verdict",
            .hint = NULL,
            .func = &cmd_mihc_asrcstat,
        },
        {
            .command = "mihc_rawstat",
            .help = "RAW cb tap: last buffer received by bt_a2d_sink_raw_data_cb",
            .hint = NULL,
            .func = &cmd_mihc_rawstat,
        },
        {
            .command = "mihc_queuestat",
            .help = "Queue tap: data_q send/drop counters (detects packet drops)",
            .hint = NULL,
            .func = &cmd_mihc_queuestat,
        },
        {
            .command = "mihc_selfplay",
            .help = "Decode embedded OTA payloads on-device (no phone)",
            .hint = NULL,
            .func = &cmd_mihc_selfplay,
        },
        {
            .command = "mihc_dac",
            .help = "Play decoded PCM straight to DAC (bypass GMF)",
            .hint = NULL,
            .func = &cmd_mihc_dac,
        },
        {
            .command = "mihc_one",
            .help = "Decode one real live payload, print first 16 samples",
            .hint = NULL,
            .func = &cmd_mihc_one,
        },
        {
            .command = "mihc_dump",
            .help = "Print captured live OTA payloads as hex",
            .hint = NULL,
            .func = &cmd_mihc_dump,
        },
        {
            .command = "mihc_sine",
            .help = "Play 1kHz sine straight to DAC (control test)",
            .hint = NULL,
            .func = &cmd_mihc_sine,
        },
        {
            .command = "log_level",
            .help = "Set runtime log level for a tag or *",
            .hint = "<tag|*> <level>",
            .func = &log_level,
        },
        {
            .command = "mihc_stream",
            .help = "Stream new live OTA payloads to UART0 as binary FEC frames",
            .hint = NULL,
            .func = &cmd_mihc_stream,
        }};

    for (int i = 0; i < sizeof(cmds) / sizeof(esp_console_cmd_t); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void cli_init()
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "BTAudio >";
    /* mihc_one / mihc_selfplay call into mihc_celt_decode_with_ec, whose local
     * buffers (freq_buf 7.5KB + X + band tables) blow the 4KB default REPL stack. */
    repl_config.task_stack_size = 32768;
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif  /* CONFIG_ESP_CONSOLE_UART */

    cli_register_sys();
    cli_register_bt();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void cli_bt_device_found(const char *name, const uint8_t *bda)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    if (strlen(target_device_name) > 0) {
        if (strstr(name, target_device_name) != NULL) {
            ESP_LOGI(TAG, "Found target device '%s', initiating auto-connect...", target_device_name);
            esp_bt_audio_classic_discovery_stop();
            esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, (uint8_t *)bda);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Connecting to %s...\n", name);
                memset(target_device_name, 0, sizeof(target_device_name));
            } else {
                ESP_LOGE(TAG, "Failed to connect to %s: %s\n", name, esp_err_to_name(ret));
            }
        }
    }
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
}

void cli_bt_device_conn_st_chg(const uint8_t *bda, bool connected)
{
    if (connected) {
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        memset(target_device_bda, 0, sizeof(target_device_bda));
    }
}
