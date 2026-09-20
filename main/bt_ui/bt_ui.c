/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "bt_ui.h"
#include "lvgl.h"
#include "jpeg_decoder.h"
#include "soc/soc_caps.h"
#if SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_playback.h"
#include "esp_bt_audio_tel.h"

#define UI_COVER_QUEUE_SIZE       2
#define UI_COVER_TASK_STACK_SIZE  8192
#define COVER_DECODE_MAX_PIXELS   (1024u * 1024u)
#define COVER_HW_JPEG_TIMEOUT_MS  500
#define UI_COVER_TASK_PRIO        5
#define UI_COVER_TASK_CORE_ID     1
/* Half-screen RGB888 double buffers starve the RGB bounce ISR during tab swipes. */
#define LVGL_DRAW_BUF_LINES       40
#define LVGL_TASK_STACK_SIZE      24576
#define LVGL_TASK_AFFINITY        0

#define BT_UI_FONT_TEXT        (&lv_font_notosanssc_regular_28)
#define BT_UI_FONT_ICON        (&lv_font_montserrat_28)
#define BT_UI_FONT_COVER_ICON  (&lv_font_montserrat_32)

/* Landscape (800x480) layout */
#define COVER_SIZE        360
#define COVER_IMG_SIZE    340
#define MEDIA_LEFT_PAD    32
#define MEDIA_GAP         32
#define RIGHT_PANEL_X     (MEDIA_LEFT_PAD + COVER_SIZE + MEDIA_GAP)
#define RIGHT_PANEL_W     (BT_UI_WIDTH - RIGHT_PANEL_X - MEDIA_LEFT_PAD)
#define BTN_ROW_HEIGHT    88
#define BTN_W             92
#define BTN_H             68
#define CHANNEL_ROW_H     32
#define CHANNEL_DOT_SIZE  18
#define CHANNEL_DOT_GAP   18
#define NAME_AREA_LINES   5
#define NAME_AREA_H       240
#define DIALER_KEYPAD_SZ  92
#define DIALER_KEY_GAP    16
#define DIALER_START_Y    82
#define DIALER_ROW_GAP    28

#define CONTENT_H          BT_UI_HEIGHT
#define VOL_BAR_W          6
#define VOL_BAR_H          (BT_UI_HEIGHT / 5)
#define VOL_BAR_RIGHT_GAP  12
#define DIALER_BUF_SIZE  32

/**
 * @brief  Cover image payload queued to the UI task.
 */
typedef struct {
    uint8_t *data;  /*!< Encoded image buffer owned by the UI task */
    size_t   size;  /*!< Encoded image buffer size in bytes */
} ui_cover_msg_t;

typedef struct {
    uint8_t          *pixels;  /*!< Decoded RGB bitmap, owned by caller */
    uint32_t          w;       /*!< Visible width in pixels */
    uint32_t          h;       /*!< Visible height in pixels */
    uint32_t          stride;  /*!< Bytes per row */
    size_t            size;    /*!< Buffer size in bytes */
#if LVGL_VERSION_MAJOR >= 9
    lv_color_format_t cf;      /*!< LVGL color format of pixels */
#endif
} cover_decoded_t;

/**
 * @brief  Runtime state for the Bluetooth demo UI.
 */
struct bt_ui_t {
    lv_disp_t                    *disp;                      /*!< Display instance used by LVGL */
    lv_obj_t                     *splash;                    /*!< Splash screen root object */
    lv_obj_t                     *main;                      /*!< Main UI root object */
    lv_obj_t                     *tabview;                   /*!< Main tabview (tab 0 = dialer, tab 1 = media) */
    lv_obj_t                     *media;                     /*!< Media page root object */
    lv_obj_t                     *dialer;                    /*!< Dialer page root object */
    lv_obj_t                     *volume_bar;                /*!< Floating volume indicator */
    int                           volume;                    /*!< Tracked volume level */
    bool                          suppress_track_metadata;   /*!< Keep stream-state text while BIS is active */
    esp_bt_audio_stream_handle_t  stream;                    /*!< Stream used to derive media placeholder state */
    QueueHandle_t                 cover_queue;               /*!< Queue carrying cover image updates */
};

/**
 * @brief  Cached LVGL widgets used by the dialer page.
 */
typedef struct {
    lv_obj_t *number_label;    /*!< Dialed number and call-state label */
    lv_obj_t *call_btn;        /*!< Dial/end-call button */
    lv_obj_t *call_btn_label;  /*!< Icon label inside call_btn */
    lv_obj_t *answer_btn;      /*!< Answer button shown for incoming calls */
    lv_obj_t *back_btn;        /*!< Backspace button */
    void     *call_ctx;        /*!< Pointer to dialer_call_ctx_t */
} bt_ui_dialer_refs_t;

/**
 * @brief  Dialer callback context and current call state.
 */
typedef struct {
    void (*call_cb)(const char *number, void *ctx);  /*!< Dial callback */
    void *call_cb_ctx;                               /*!< Context passed to call_cb */
    void (*end_call_cb)(void *ctx);                  /*!< End-call callback */
    void *end_call_cb_ctx;                           /*!< Context passed to end_call_cb */
    void (*answer_call_cb)(void *ctx);               /*!< Answer-call callback */
    void *answer_call_cb_ctx;                        /*!< Context passed to answer_call_cb */
    bool  in_call;                                   /*!< True while the dialer is in a call state */
} dialer_call_ctx_t;

/**
 * @brief  Cached LVGL widgets and callback state used by the media page.
 */
typedef struct {
    lv_obj_t *play_btn;               /*!< Play/pause button */
    lv_obj_t *title_label;            /*!< Track title label */
    lv_obj_t *artist_label;           /*!< Track artist label */
    lv_obj_t *cover_cont;             /*!< Cover-art container */
    lv_obj_t *cover_img;              /*!< Decoded cover-art image */
    lv_obj_t *cover_placeholder;      /*!< Placeholder shown when no cover art is available */
    lv_obj_t *cover_type_label;       /*!< Fallback text/icon label for stream type */
    lv_obj_t *cover_type_desc_label;  /*!< Broadcast location description label */
    lv_obj_t *cover_type_img;         /*!< Stream-type image label */
    lv_obj_t *left_channel_dot;       /*!< Left-channel position indicator */
    lv_obj_t *right_channel_dot;      /*!< Right-channel position indicator */
    uint8_t  *cover_data;             /*!< Decoded cover-art buffer */
    size_t    cover_size;             /*!< Decoded cover-art buffer size */
#if LVGL_VERSION_MAJOR >= 9
    lv_image_dsc_t  cover_dsc;      /*!< Persistent descriptor for LVGL (src points here) */
#else
    lv_img_dsc_t  cover_img_dsc;   /*!< Persistent descriptor for LVGL (src points here) */
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    void (*play_pause_cb)(bool want_play, void *ctx);  /*!< Play/pause callback */
    void *play_pause_ctx;                              /*!< Context passed to play_pause_cb */
    void (*prev_cb)(void *ctx);                        /*!< Previous-track callback */
    void (*next_cb)(void *ctx);                        /*!< Next-track callback */
    void *prev_next_ctx;                               /*!< Context passed to prev_cb and next_cb */
} bt_ui_media_refs_t;

LV_FONT_DECLARE(lv_font_notosanssc_regular_28)

#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
LV_IMAGE_DECLARE(cis_stream_icon);
LV_IMAGE_DECLARE(bis_stream_icon);
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */

static char dialer_number_buf[DIALER_BUF_SIZE];
static lv_obj_t *dialer_number_label;
static lv_timer_t *s_vol_bar_hide_timer = NULL;
static const char *TAG = "BT_UI";

static void vol_bar_hide_timer_cb(lv_timer_t *t)
{
    lv_obj_t *bar = (lv_obj_t *)lv_timer_get_user_data(t);
    if (bar != NULL) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_del(t);
    s_vol_bar_hide_timer = NULL;
}

static uint32_t bt_ui_channel_locations_from_tech(esp_bt_audio_tech_t tech)
{
    if (tech == ESP_BT_AUDIO_TECH_CLASSIC) {
        return ESP_BT_AUDIO_AUDIO_LOC_FRONT_LEFT | ESP_BT_AUDIO_AUDIO_LOC_FRONT_RIGHT;
    }
    if (tech != ESP_BT_AUDIO_TECH_LE) {
        return 0;
    }
#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
    uint32_t locations = 0;
#if CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_LEFT
    locations |= ESP_BT_AUDIO_AUDIO_LOC_FRONT_LEFT;
#endif  /* CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_LEFT */
#if CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_RIGHT
    locations |= ESP_BT_AUDIO_AUDIO_LOC_FRONT_RIGHT;
#endif  /* CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_RIGHT */
#if CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_LEFT_RIGHT
    locations |= ESP_BT_AUDIO_AUDIO_LOC_FRONT_LEFT | ESP_BT_AUDIO_AUDIO_LOC_FRONT_RIGHT;
#endif  /* CONFIG_GMF_EXAMPLE_LE_LOCATION_FRONT_LEFT_RIGHT */
    return locations;
#else
    return 0;
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */
}

static bool bt_ui_default_channel_is_le_audio(void)
{
#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
    return true;
#else
    return false;
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */
}

static void bt_ui_media_set_channel_locations(lv_obj_t *media_root, uint32_t locations, bool le_audio)
{
    if (media_root == NULL) {
        return;
    }
    bt_ui_media_refs_t *refs = (bt_ui_media_refs_t *)lv_obj_get_user_data(media_root);
    if (refs == NULL || refs->left_channel_dot == NULL || refs->right_channel_dot == NULL) {
        return;
    }

    lv_color_t bright = lv_color_hex(le_audio ? 0xffd54f : 0x42a5f5);
    lv_color_t gray = lv_color_hex(0x657080);
    lv_obj_set_style_bg_color(refs->left_channel_dot,
                              (locations & ESP_BT_AUDIO_AUDIO_LOC_FRONT_LEFT) ? bright : gray, 0);
    lv_obj_set_style_bg_color(refs->right_channel_dot,
                              (locations & ESP_BT_AUDIO_AUDIO_LOC_FRONT_RIGHT) ? bright : gray, 0);
}

static void btn_play_pause_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *root = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
    bt_ui_media_refs_t *refs = root ? (bt_ui_media_refs_t *)lv_obj_get_user_data(root) : NULL;
    if (refs && refs->play_pause_cb) {
        /* VALUE_CHANGED runs after toggle: checked => user requested play */
        bool want_play = lv_obj_has_state(btn, LV_STATE_CHECKED);
        refs->play_pause_cb(want_play, refs->play_pause_ctx);
    }
}

static void btn_prev_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *root = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
    bt_ui_media_refs_t *refs = root ? (bt_ui_media_refs_t *)lv_obj_get_user_data(root) : NULL;
    if (refs && refs->prev_cb) {
        refs->prev_cb(refs->prev_next_ctx);
    }
}

static void btn_next_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *root = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
    bt_ui_media_refs_t *refs = root ? (bt_ui_media_refs_t *)lv_obj_get_user_data(root) : NULL;
    if (refs && refs->next_cb) {
        refs->next_cb(refs->prev_next_ctx);
    }
}

static void dialer_digit_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : NULL;
    if (txt == NULL || dialer_number_label == NULL) {
        return;
    }
    size_t len = lv_strlen(dialer_number_buf);
    if (len + 2 >= DIALER_BUF_SIZE) {
        return;
    }
    if (txt[0] != '\0') {
        dialer_number_buf[len] = txt[0];
        dialer_number_buf[len + 1] = '\0';
    }
    lv_label_set_text(dialer_number_label, dialer_number_buf[0] ? dialer_number_buf : "Enter Number");
}

static void dialer_back_click_cb(lv_event_t *e)
{
    if (dialer_number_label == NULL) {
        return;
    }
    size_t len = lv_strlen(dialer_number_buf);
    if (len > 0) {
        dialer_number_buf[len - 1] = '\0';
    }
    lv_label_set_text(dialer_number_label, dialer_number_buf[0] ? dialer_number_buf : "Enter Number");
}

static void dialer_call_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    dialer_call_ctx_t *ctx = (dialer_call_ctx_t *)lv_obj_get_user_data(btn);
    if (ctx == NULL) {
        return;
    }
    if (ctx->in_call) {
        if (ctx->end_call_cb) {
            ctx->end_call_cb(ctx->end_call_cb_ctx);
        }
        return;
    }
    if (ctx->call_cb) {
        ctx->call_cb(dialer_number_buf, ctx->call_cb_ctx);
    }
}

static void dialer_answer_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    dialer_call_ctx_t *ctx = (dialer_call_ctx_t *)lv_obj_get_user_data(btn);
    if (ctx && ctx->answer_call_cb) {
        ctx->answer_call_cb(ctx->answer_call_cb_ctx);
    }
}

static lv_obj_t *bt_ui_volume_bar_create(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, VOL_BAR_W, VOL_BAR_H);
    lv_obj_align(cont, LV_ALIGN_RIGHT_MID, -VOL_BAR_RIGHT_GAP, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 2, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x505060), 0);

    lv_obj_t *fill = lv_obj_create(cont);
    lv_obj_remove_style_all(fill);
    lv_obj_set_width(fill, VOL_BAR_W);
    lv_obj_set_height(fill, 0);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(fill, 2, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_pad_all(fill, 0, 0);

    lv_obj_set_user_data(cont, fill);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);
    return cont;
}

static void bt_ui_volume_bar_set_level(lv_obj_t *vol_bar, int level_percent)
{
    if (vol_bar == NULL) {
        return;
    }
    if (level_percent < 0) {
        level_percent = 0;
    }
    if (level_percent > 100) {
        level_percent = 100;
    }
    lv_obj_t *fill = (lv_obj_t *)lv_obj_get_user_data(vol_bar);
    if (fill == NULL) {
        return;
    }
    int32_t h = lv_obj_get_height(vol_bar);
    int32_t fill_h = (int32_t)((int64_t)h * level_percent / 100);
    if (fill_h < 0) {
        fill_h = 0;
    }
    lv_obj_set_height(fill, (lv_coord_t)fill_h);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_clear_flag(vol_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_vol_bar_hide_timer != NULL) {
        lv_timer_reset(s_vol_bar_hide_timer);
    } else {
        s_vol_bar_hide_timer = lv_timer_create(vol_bar_hide_timer_cb, 1000, vol_bar);
        lv_timer_set_repeat_count(s_vol_bar_hide_timer, 1);
    }
}

static lv_obj_t *bt_ui_splash_create(lv_obj_t *parent, const char *device_name)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BT_UI_WIDTH, BT_UI_HEIGHT);
    lv_obj_center(root);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x102840), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(root);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, BT_UI_FONT_TEXT, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(label, "Device Name:\n%s", device_name && device_name[0] ? device_name : "(BT)");
    lv_obj_center(label);
    return root;
}

static void bt_ui_media_set_playing(lv_obj_t *media_root, bool playing)
{
    if (media_root == NULL) {
        return;
    }
    bt_ui_media_refs_t *refs = (bt_ui_media_refs_t *)lv_obj_get_user_data(media_root);
    if (refs == NULL || refs->play_btn == NULL) {
        return;
    }
    lv_obj_t *lbl = lv_obj_get_child(refs->play_btn, 0);
    if (lbl != NULL && lv_obj_check_type(lbl, &lv_label_class)) {
        lv_label_set_text(lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if (playing) {
            lv_obj_add_state(refs->play_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(refs->play_btn, LV_STATE_CHECKED);
        }
    }
}

static void bt_ui_media_set_track(lv_obj_t *media_root, const char *title, const char *artist)
{
    if (media_root == NULL) {
        return;
    }
    bt_ui_media_refs_t *refs = (bt_ui_media_refs_t *)lv_obj_get_user_data(media_root);
    if (refs == NULL || refs->title_label == NULL) {
        return;
    }
    /* NULL means leave current; UI labels hold the state across partial updates */
    if (title != NULL) {
        lv_label_set_text(refs->title_label, title[0] != '\0' ? title : "No music playing");
    }
    if (refs->artist_label != NULL && artist != NULL) {
        lv_label_set_text(refs->artist_label, artist[0] != '\0' ? artist : "");
    }
}

static void bt_ui_media_set_stream_type(lv_obj_t *media_root, esp_bt_audio_stream_profile_t profile)
{
    if (media_root == NULL) {
        return;
    }
    bt_ui_media_refs_t *refs = (bt_ui_media_refs_t *)lv_obj_get_user_data(media_root);
    if (refs == NULL || refs->cover_placeholder == NULL || refs->cover_type_label == NULL) {
        return;
    }

    const lv_image_dsc_t *icon = NULL;
#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
    if (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_UNICAST) {
        icon = &cis_stream_icon;
    } else if (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST) {
        icon = &bis_stream_icon;
    }
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */
    if (icon != NULL && refs->cover_type_img != NULL) {
        lv_image_set_src(refs->cover_type_img, icon);
        lv_obj_clear_flag(refs->cover_type_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(refs->cover_type_label, LV_OBJ_FLAG_HIDDEN);
        if (refs->cover_type_desc_label != NULL) {
            lv_obj_add_flag(refs->cover_type_desc_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (refs->cover_img != NULL) {
            lv_obj_add_flag(refs->cover_img, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (refs->cover_type_img != NULL) {
            lv_obj_add_flag(refs->cover_type_img, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(refs->cover_type_label, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(refs->cover_type_label, lv_color_hex(0xa8b8c8), 0);
        lv_obj_clear_flag(refs->cover_type_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(refs->cover_placeholder, LV_OBJ_FLAG_HIDDEN);
}

static uint8_t cover_jpeg_sof(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }
    size_t ofs = 2;
    while (ofs + 4 <= size) {
        if (data[ofs] != 0xFF) {
            ofs++;
            continue;
        }
        uint8_t mk = data[ofs + 1];
        if (mk == 0xFF) {
            ofs++;
            continue;
        }
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) {
            ofs += 2;
            continue;
        }
        if (mk == 0xD9) {
            break;
        }
        uint16_t len = ((uint16_t)data[ofs + 2] << 8) | data[ofs + 3];
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            return mk;
        }
        if (len < 2) {
            break;
        }
        ofs += 2u + (size_t)len;
    }
    return 0;
}

static bool cover_want_rgb888(void)
{
#if LVGL_VERSION_MAJOR >= 9
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        return lv_display_get_color_format(disp) == LV_COLOR_FORMAT_RGB888;
    }
#endif
    return false;
}

#if SOC_JPEG_DECODE_SUPPORTED
static jpeg_decoder_handle_t s_cover_jpeg = NULL;
static uint8_t *s_cover_jpeg_in = NULL;
static size_t s_cover_jpeg_in_cap = 0;

static uint32_t cover_align16(uint32_t value)
{
    return (value + 15u) & ~15u;
}

static bool cover_decode_hw_jpeg(const uint8_t *jpeg, size_t jpeg_len, cover_decoded_t *out)
{
    if (jpeg == NULL || jpeg_len == 0 || out == NULL) {
        return false;
    }
    if (s_cover_jpeg == NULL) {
        jpeg_decode_engine_cfg_t eng_cfg = {
            .intr_priority = 0,
            .timeout_ms = COVER_HW_JPEG_TIMEOUT_MS,
        };
        if (jpeg_new_decoder_engine(&eng_cfg, &s_cover_jpeg) != ESP_OK || s_cover_jpeg == NULL) {
            ESP_LOGW(TAG, "Cover HW JPEG engine create failed, fallback to TJpgDec");
            return false;
        }
    }

    jpeg_decode_picture_info_t info = {0};
    esp_err_t err = jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_len, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cover HW JPEG get_info failed: %s", esp_err_to_name(err));
        return false;
    }
    if (info.width == 0 || info.height == 0 ||
        (uint64_t)info.width * (uint64_t)info.height > COVER_DECODE_MAX_PIXELS) {
        ESP_LOGW(TAG, "Cover HW JPEG skipped, size %" PRIu32 "x%" PRIu32, info.width, info.height);
        return false;
    }

    if (s_cover_jpeg_in == NULL || s_cover_jpeg_in_cap < jpeg_len) {
        if (s_cover_jpeg_in != NULL) {
            free(s_cover_jpeg_in);
            s_cover_jpeg_in = NULL;
            s_cover_jpeg_in_cap = 0;
        }
        jpeg_decode_memory_alloc_cfg_t in_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        s_cover_jpeg_in = (uint8_t *)jpeg_alloc_decoder_mem(jpeg_len, &in_cfg, &s_cover_jpeg_in_cap);
        if (s_cover_jpeg_in == NULL) {
            ESP_LOGE(TAG, "Cover HW JPEG input alloc failed, size %u", (unsigned)jpeg_len);
            return false;
        }
    }
    memcpy(s_cover_jpeg_in, jpeg, jpeg_len);

    const uint32_t padded_w = cover_align16(info.width);
    const uint32_t padded_h = cover_align16(info.height);
    const size_t out_bytes = (size_t)padded_w * (size_t)padded_h * 3u;
    jpeg_decode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_cap = 0;
    uint8_t *pixels = (uint8_t *)jpeg_alloc_decoder_mem(out_bytes, &out_cfg, &out_cap);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "Cover HW JPEG output alloc failed, need %u", (unsigned)out_bytes);
        return false;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        /* LVGL RGB888 is R,G,B in memory; LCD_COLOR_FMT_RGB888 on this panel is BGR24. */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };
    uint32_t decoded_size = 0;
    err = jpeg_decoder_process(s_cover_jpeg, &dec_cfg,
                               s_cover_jpeg_in, (uint32_t)jpeg_len,
                               pixels, (uint32_t)out_cap, &decoded_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cover HW JPEG decode failed: %s", esp_err_to_name(err));
        free(pixels);
        return false;
    }

    out->pixels = pixels;
    out->w = info.width;
    out->h = info.height;
    out->stride = padded_w * 3u;
    out->size = out_cap;
#if LVGL_VERSION_MAJOR >= 9
    out->cf = LV_COLOR_FORMAT_RGB888;
#endif
    ESP_LOGI(TAG, "Cover HW JPEG decoded %" PRIu32 "x%" PRIu32 " stride=%" PRIu32 " out=%" PRIu32,
             out->w, out->h, out->stride, decoded_size);
    return true;
}
#endif  /* SOC_JPEG_DECODE_SUPPORTED */

static bool cover_decode_tjpgd(const uint8_t *jpeg, size_t jpeg_len, bool rgb888, cover_decoded_t *out)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)jpeg_len,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = rgb888 ? JPEG_IMAGE_FORMAT_RGB888 : JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = rgb888 ? 0 : 1},
        .advanced = {.working_buffer = NULL, .working_buffer_size = 0},
        .priv = {0},
    };
    esp_jpeg_image_output_t info;
    esp_err_t err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cover TJpgDec get_info failed: %s (need baseline SOF0 JPEG)",
                 esp_err_to_name(err));
        return false;
    }

    uint32_t src_w = (uint32_t)info.width;
    uint32_t src_h = (uint32_t)info.height;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    if (src_w > COVER_IMG_SIZE || src_h > COVER_IMG_SIZE) {
        if (src_w / 2 <= COVER_IMG_SIZE && src_h / 2 <= COVER_IMG_SIZE) {
            scale = JPEG_IMAGE_SCALE_1_2;
        } else if (src_w / 4 <= COVER_IMG_SIZE && src_h / 4 <= COVER_IMG_SIZE) {
            scale = JPEG_IMAGE_SCALE_1_4;
        } else {
            scale = JPEG_IMAGE_SCALE_1_8;
        }
    }
    cfg.out_scale = scale;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        ESP_LOGE(TAG, "Cover TJpgDec get_info scale %d failed", (int)scale);
        return false;
    }

    uint8_t *outbuf = (uint8_t *)heap_caps_malloc_prefer(info.output_len, 2,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                         MALLOC_CAP_DEFAULT);
    if (outbuf == NULL) {
        ESP_LOGE(TAG, "Cover TJpgDec malloc(%u) failed", (unsigned)info.output_len);
        return false;
    }
    cfg.outbuf = outbuf;
    cfg.outbuf_size = (uint32_t)info.output_len;
    esp_jpeg_image_output_t outimg;
    err = esp_jpeg_decode(&cfg, &outimg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cover TJpgDec decode failed: %s", esp_err_to_name(err));
        free(outbuf);
        return false;
    }

    out->pixels = outbuf;
    out->w = outimg.width;
    out->h = outimg.height;
    out->stride = (uint32_t)outimg.width * (rgb888 ? 3u : 2u);
    out->size = outimg.output_len;
#if LVGL_VERSION_MAJOR >= 9
    out->cf = rgb888 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565_SWAPPED;
#endif
    ESP_LOGI(TAG, "Cover TJpgDec decoded %ux%u scale=%d rgb888=%d",
             (unsigned)out->w, (unsigned)out->h, (int)scale, (int)rgb888);
    return true;
}

static bool cover_decode_image(const uint8_t *data, size_t size, cover_decoded_t *out)
{
    if (data == NULL || size == 0 || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        ESP_LOGE(TAG, "Cover is PNG (%u bytes), decoder only supports JPEG", (unsigned)size);
        return false;
    }
    uint8_t sof = cover_jpeg_sof(data, size);
    if (sof == 0) {
        ESP_LOGE(TAG, "Cover is not a JPEG (size %u, head %02X %02X %02X %02X)",
                 (unsigned)size,
                 size > 0 ? data[0] : 0, size > 1 ? data[1] : 0,
                 size > 2 ? data[2] : 0, size > 3 ? data[3] : 0);
        return false;
    }
    if (sof == 0xC2) {
        ESP_LOGW(TAG, "Cover JPEG is progressive (SOF2), hardware/TJpgDec only support baseline");
    } else if (sof != 0xC0) {
        ESP_LOGW(TAG, "Cover JPEG SOF=0x%02X (only SOF0 baseline is supported)", sof);
    }

    bool rgb888 = cover_want_rgb888();
#if SOC_JPEG_DECODE_SUPPORTED
    if (sof == 0xC0 && cover_decode_hw_jpeg(data, size, out)) {
        return true;
    }
#endif
    return cover_decode_tjpgd(data, size, rgb888, out);
}

static void bt_ui_media_apply_cover(lv_obj_t *media_root, cover_decoded_t *dec)
{
    bt_ui_media_refs_t *refs = (bt_ui_media_refs_t *)lv_obj_get_user_data(media_root);
    if (refs == NULL || refs->cover_cont == NULL || dec == NULL || dec->pixels == NULL) {
        if (dec != NULL && dec->pixels != NULL) {
            free(dec->pixels);
            dec->pixels = NULL;
        }
        return;
    }

    if (refs->cover_data != NULL) {
        free(refs->cover_data);
        refs->cover_data = NULL;
        refs->cover_size = 0;
    }
    refs->cover_data = dec->pixels;
    refs->cover_size = dec->size;
    dec->pixels = NULL;

    if (refs->cover_img != NULL) {
#if LVGL_VERSION_MAJOR >= 9
        memset(&refs->cover_dsc, 0, sizeof(refs->cover_dsc));
        refs->cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        refs->cover_dsc.header.cf = dec->cf;
        refs->cover_dsc.header.flags = 0;
        refs->cover_dsc.header.w = (int32_t)dec->w;
        refs->cover_dsc.header.h = (int32_t)dec->h;
        refs->cover_dsc.header.stride = dec->stride;
        refs->cover_dsc.data_size = (uint32_t)dec->size;
        refs->cover_dsc.data = refs->cover_data;
        lv_image_set_src(refs->cover_img, &refs->cover_dsc);
        lv_image_set_inner_align(refs->cover_img, LV_IMAGE_ALIGN_STRETCH);
#else
        memset(&refs->cover_img_dsc, 0, sizeof(refs->cover_img_dsc));
        refs->cover_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        refs->cover_img_dsc.header.w = (int32_t)dec->w;
        refs->cover_img_dsc.header.h = (int32_t)dec->h;
        refs->cover_img_dsc.header.stride = dec->stride;
        refs->cover_img_dsc.data_size = (uint32_t)dec->size;
        refs->cover_img_dsc.data = refs->cover_data;
        lv_img_set_src(refs->cover_img, &refs->cover_img_dsc);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
        lv_obj_clear_flag(refs->cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(refs->cover_img, -1);
    }
    if (refs->cover_placeholder != NULL) {
        lv_obj_add_flag(refs->cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "Cover visible %ux%u stride=%u",
             (unsigned)dec->w, (unsigned)dec->h, (unsigned)dec->stride);
}

static lv_obj_t *bt_ui_media_create(lv_obj_t *parent,
                                    void (*play_pause_cb)(bool want_play, void *ctx), void *play_pause_ctx,
                                    void (*prev_cb)(void *ctx), void (*next_cb)(void *ctx), void *prev_next_ctx)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BT_UI_WIDTH, CONTENT_H);
    lv_obj_center(root);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x102840), 0);

    lv_obj_t *cover_cont = lv_obj_create(root);
    lv_obj_remove_style_all(cover_cont);
    lv_obj_set_size(cover_cont, COVER_SIZE, COVER_SIZE);
    lv_obj_align(cover_cont, LV_ALIGN_LEFT_MID, MEDIA_LEFT_PAD, 0);
    lv_obj_clear_flag(cover_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cover_placeholder = lv_obj_create(cover_cont);
    lv_obj_remove_style_all(cover_placeholder);
    lv_obj_set_size(cover_placeholder, COVER_IMG_SIZE, COVER_IMG_SIZE);
    lv_obj_align(cover_placeholder, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(cover_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cover_placeholder, lv_color_hex(0x3d4f63), 0);
    lv_obj_set_style_radius(cover_placeholder, 8, 0);
    lv_obj_clear_flag(cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cover_type_label = lv_label_create(cover_placeholder);
    lv_label_set_text(cover_type_label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(cover_type_label, BT_UI_FONT_COVER_ICON, 0);
    lv_obj_set_style_text_color(cover_type_label, lv_color_hex(0xa8b8c8), 0);
    lv_obj_center(cover_type_label);

    lv_obj_t *cover_type_desc_label = lv_label_create(cover_placeholder);
    lv_label_set_text(cover_type_desc_label, "");
    lv_obj_set_style_text_font(cover_type_desc_label, BT_UI_FONT_TEXT, 0);
    lv_obj_set_style_text_color(cover_type_desc_label, lv_color_hex(0xa8b8c8), 0);
    lv_obj_align_to(cover_type_desc_label, cover_type_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_add_flag(cover_type_desc_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cover_type_img = lv_image_create(cover_placeholder);
    lv_obj_remove_style_all(cover_type_img);
    lv_obj_set_size(cover_type_img, 280, 187);
    lv_obj_center(cover_type_img);
    lv_obj_add_flag(cover_type_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cover_type_img, LV_OBJ_FLAG_SCROLLABLE);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *cover_img = lv_image_create(cover_cont);
    lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_STRETCH);
#else
    lv_obj_t *cover_img = lv_img_create(cover_cont);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(cover_img, COVER_IMG_SIZE, COVER_IMG_SIZE);
    lv_obj_align(cover_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cover_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *right_panel = lv_obj_create(root);
    lv_obj_remove_style_all(right_panel);
    lv_obj_set_size(right_panel, RIGHT_PANEL_W, COVER_SIZE);
    lv_obj_align(right_panel, LV_ALIGN_LEFT_MID, RIGHT_PANEL_X, 0);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_cont = lv_obj_create(right_panel);
    lv_obj_remove_style_all(name_cont);
    lv_obj_set_size(name_cont, RIGHT_PANEL_W, NAME_AREA_H);
    lv_obj_clear_flag(name_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_label = lv_label_create(name_cont);
    lv_label_set_text(name_label, "-----");
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xe8eef4), 0);
    lv_obj_set_width(name_label, RIGHT_PANEL_W);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(name_label, BT_UI_FONT_TEXT, 0);
    lv_obj_set_style_text_line_space(name_label, -4, 0);

    lv_obj_t *artist_label = lv_label_create(name_cont);
    lv_label_set_text(artist_label, "");
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(artist_label, lv_color_hex(0x9098a0), 0);
    lv_obj_set_width(artist_label, RIGHT_PANEL_W);
    lv_obj_align_to(artist_label, name_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 44);
    lv_obj_set_style_text_font(artist_label, BT_UI_FONT_TEXT, 0);
    lv_obj_set_style_text_line_space(artist_label, -4, 0);

    lv_obj_t *spacer = lv_obj_create(right_panel);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *channel_cont = lv_obj_create(right_panel);
    lv_obj_remove_style_all(channel_cont);
    lv_obj_set_size(channel_cont, RIGHT_PANEL_W, CHANNEL_ROW_H);
    lv_obj_set_flex_flow(channel_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(channel_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(channel_cont, CHANNEL_DOT_GAP, 0);
    lv_obj_clear_flag(channel_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_channel_dot = lv_obj_create(channel_cont);
    lv_obj_remove_style_all(left_channel_dot);
    lv_obj_set_size(left_channel_dot, CHANNEL_DOT_SIZE, CHANNEL_DOT_SIZE);
    lv_obj_set_style_radius(left_channel_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(left_channel_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(left_channel_dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *right_channel_dot = lv_obj_create(channel_cont);
    lv_obj_remove_style_all(right_channel_dot);
    lv_obj_set_size(right_channel_dot, CHANNEL_DOT_SIZE, CHANNEL_DOT_SIZE);
    lv_obj_set_style_radius(right_channel_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(right_channel_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(right_channel_dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_cont = lv_obj_create(right_panel);
    lv_obj_remove_style_all(btn_cont);
    lv_obj_set_size(btn_cont, RIGHT_PANEL_W, BTN_ROW_HEIGHT);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *btn_prev = lv_button_create(btn_cont);
#else
    lv_obj_t *btn_prev = lv_btn_create(btn_cont);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(btn_prev, BTN_W, BTN_H);
    lv_obj_set_style_shadow_width(btn_prev, 0, 0);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(lbl_prev, BT_UI_FONT_ICON, 0);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(btn_prev, btn_prev_click_cb, LV_EVENT_CLICKED, NULL);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *btn_play = lv_button_create(btn_cont);
#else
    lv_obj_t *btn_play = lv_btn_create(btn_cont);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(btn_play, BTN_W, BTN_H);
    lv_obj_set_style_shadow_width(btn_play, 0, 0);
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_t *lbl_play = lv_label_create(btn_play);
    lv_label_set_text(lbl_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_play, BT_UI_FONT_ICON, 0);
    lv_obj_center(lbl_play);
    lv_obj_add_event_cb(btn_play, btn_play_pause_click_cb, LV_EVENT_VALUE_CHANGED, NULL);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *btn_next = lv_button_create(btn_cont);
#else
    lv_obj_t *btn_next = lv_btn_create(btn_cont);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(btn_next, BTN_W, BTN_H);
    lv_obj_set_style_shadow_width(btn_next, 0, 0);
    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(lbl_next, BT_UI_FONT_ICON, 0);
    lv_obj_center(lbl_next);
    lv_obj_add_event_cb(btn_next, btn_next_click_cb, LV_EVENT_CLICKED, NULL);

    static bt_ui_media_refs_t s_media_refs;
    s_media_refs.play_btn = btn_play;
    s_media_refs.title_label = name_label;
    s_media_refs.artist_label = artist_label;
    s_media_refs.cover_cont = cover_cont;
    s_media_refs.cover_img = cover_img;
    s_media_refs.cover_placeholder = cover_placeholder;
    s_media_refs.cover_type_label = cover_type_label;
    s_media_refs.cover_type_desc_label = cover_type_desc_label;
    s_media_refs.cover_type_img = cover_type_img;
    s_media_refs.left_channel_dot = left_channel_dot;
    s_media_refs.right_channel_dot = right_channel_dot;
    s_media_refs.cover_data = NULL;
    s_media_refs.cover_size = 0;
    s_media_refs.play_pause_cb = play_pause_cb;
    s_media_refs.play_pause_ctx = play_pause_ctx;
    s_media_refs.prev_cb = prev_cb;
    s_media_refs.next_cb = next_cb;
    s_media_refs.prev_next_ctx = prev_next_ctx;
    lv_obj_set_user_data(root, &s_media_refs);
    bt_ui_media_set_channel_locations(root, bt_ui_channel_locations_from_tech(ESP_BT_AUDIO_TECH_UNKNOWN),
                                      bt_ui_default_channel_is_le_audio());

    return root;
}

static lv_obj_t *bt_ui_dialer_create(lv_obj_t *parent,
                                     void (*call_cb)(const char *number, void *ctx), void *call_cb_ctx,
                                     void (*end_call_cb)(void *ctx), void *end_call_cb_ctx,
                                     void (*answer_call_cb)(void *ctx), void *answer_call_cb_ctx)
{
    dialer_number_buf[0] = '\0';
    dialer_number_label = NULL;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BT_UI_WIDTH, CONTENT_H);
    lv_obj_center(root);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x102840), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    dialer_number_label = lv_label_create(root);
    lv_label_set_text(dialer_number_label, "Enter Number");
    lv_obj_set_style_text_color(dialer_number_label, lv_color_hex(0xe8eef4), 0);
    lv_obj_set_width(dialer_number_label, BT_UI_WIDTH / 2 - 64);
    lv_label_set_long_mode(dialer_number_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(dialer_number_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(dialer_number_label, LV_ALIGN_TOP_RIGHT, -44, 88);
    lv_obj_set_style_text_font(dialer_number_label, BT_UI_FONT_TEXT, 0);

    static const char *keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    const int cols = 3, rows = 4;
    const int key_sz = DIALER_KEYPAD_SZ;
    int start_x = 46;
    int start_y = (CONTENT_H - (rows * key_sz + (rows - 1) * DIALER_KEY_GAP)) / 2;
    const lv_color_t key_gray = lv_color_hex(0x6a6a6a);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
#if LVGL_VERSION_MAJOR >= 9
            lv_obj_t *btn = lv_button_create(root);
#else
            lv_obj_t *btn = lv_btn_create(root);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
            lv_obj_set_size(btn, key_sz, key_sz);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(btn, key_gray, 0);
            lv_obj_set_pos(btn, start_x + c * (key_sz + DIALER_KEY_GAP), start_y + r * (key_sz + DIALER_KEY_GAP));
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, keys[i]);
            lv_obj_center(lbl);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xe8eef4), 0);
            lv_obj_set_style_text_font(lbl, BT_UI_FONT_TEXT, 0);
            lv_obj_add_event_cb(btn, dialer_digit_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *call_btn = lv_button_create(root);
#else
    lv_obj_t *call_btn = lv_btn_create(root);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(call_btn, key_sz, key_sz);
    lv_obj_set_style_shadow_width(call_btn, 0, 0);
    lv_obj_set_style_radius(call_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(call_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_pos(call_btn, BT_UI_WIDTH - 284, CONTENT_H - key_sz - 54);
    static dialer_call_ctx_t s_dialer_call_ctx;
    s_dialer_call_ctx.call_cb = call_cb;
    s_dialer_call_ctx.call_cb_ctx = call_cb_ctx;
    s_dialer_call_ctx.end_call_cb = end_call_cb;
    s_dialer_call_ctx.end_call_cb_ctx = end_call_cb_ctx;
    s_dialer_call_ctx.answer_call_cb = answer_call_cb;
    s_dialer_call_ctx.answer_call_cb_ctx = answer_call_cb_ctx;
    s_dialer_call_ctx.in_call = false;
    lv_obj_set_user_data(call_btn, &s_dialer_call_ctx);
    lv_obj_add_event_cb(call_btn, dialer_call_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *call_lbl = lv_label_create(call_btn);
    lv_label_set_text(call_lbl, LV_SYMBOL_CALL);
    lv_obj_center(call_lbl);
    lv_obj_set_style_text_color(call_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(call_lbl, BT_UI_FONT_ICON, 0);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *back_btn = lv_button_create(root);
#else
    lv_obj_t *back_btn = lv_btn_create(root);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(back_btn, key_sz, key_sz);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x795548), 0);
    lv_obj_set_pos(back_btn, BT_UI_WIDTH - 158, CONTENT_H - key_sz - 54);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_BACKSPACE);
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(back_lbl, BT_UI_FONT_ICON, 0);
    lv_obj_add_event_cb(back_btn, dialer_back_click_cb, LV_EVENT_CLICKED, NULL);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *answer_btn = lv_button_create(root);
#else
    lv_obj_t *answer_btn = lv_btn_create(root);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_size(answer_btn, key_sz, key_sz);
    lv_obj_set_style_shadow_width(answer_btn, 0, 0);
    lv_obj_set_style_radius(answer_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(answer_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_pos(answer_btn, BT_UI_WIDTH - 158, CONTENT_H - key_sz - 54);
    lv_obj_set_user_data(answer_btn, &s_dialer_call_ctx);
    lv_obj_add_event_cb(answer_btn, dialer_answer_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *answer_lbl = lv_label_create(answer_btn);
    lv_label_set_text(answer_lbl, LV_SYMBOL_CALL);
    lv_obj_center(answer_lbl);
    lv_obj_set_style_text_color(answer_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(answer_lbl, BT_UI_FONT_ICON, 0);
    lv_obj_add_flag(answer_btn, LV_OBJ_FLAG_HIDDEN);

    static bt_ui_dialer_refs_t s_dialer_refs;
    s_dialer_refs.number_label = dialer_number_label;
    s_dialer_refs.call_btn = call_btn;
    s_dialer_refs.call_btn_label = call_lbl;
    s_dialer_refs.answer_btn = answer_btn;
    s_dialer_refs.back_btn = back_btn;
    s_dialer_refs.call_ctx = &s_dialer_call_ctx;
    lv_obj_set_user_data(root, &s_dialer_refs);

    return root;
}

static void bt_ui_dialer_set_call_state(lv_obj_t *dialer_root, int state, const char *number)
{
    if (dialer_root == NULL) {
        return;
    }
    bt_ui_dialer_refs_t *refs = (bt_ui_dialer_refs_t *)lv_obj_get_user_data(dialer_root);
    if (refs == NULL || refs->number_label == NULL) {
        return;
    }
    const char *state_text = "";
    switch (state) {
        case 1:
            state_text = "Incoming";
            break;
        case 2:
            state_text = "Dialing...";
            break;
        case 3:
            state_text = "Ringing...";
            break;
        case 4:
            state_text = "In call";
            break;
        case 5:
        case 6:
        case 7:
            state_text = "Held";
            break;
        default:
            break;
    }
    if (state != 0 && number != NULL && number[0] != '\0') {
        lv_label_set_text_fmt(refs->number_label, "%s  %s", state_text, number);
    } else if (state != 0) {
        lv_label_set_text(refs->number_label, state_text);
    } else {
        dialer_number_buf[0] = '\0';
        lv_label_set_text(refs->number_label, "Enter Number");
    }
    dialer_call_ctx_t *ctx = (dialer_call_ctx_t *)refs->call_ctx;
    if (ctx != NULL) {
        ctx->in_call = state != 0;
    }

    bool incoming = (state == ESP_BT_AUDIO_CALL_STATE_INCOMING);
    bool active = (state != 0);

    if (refs->call_btn != NULL) {
        lv_obj_set_style_bg_color(refs->call_btn, lv_color_hex(active ? 0xc62828 : 0x2E7D32), 0);
    }
    if (refs->call_btn_label != NULL) {
        lv_label_set_text(refs->call_btn_label, active ? LV_SYMBOL_CLOSE : LV_SYMBOL_CALL);
    }

    if (refs->answer_btn != NULL) {
        if (incoming) {
            lv_obj_clear_flag(refs->answer_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(refs->answer_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (refs->back_btn != NULL) {
        if (active) {
            lv_obj_add_flag(refs->back_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(refs->back_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void bt_ui_cover_task(void *arg)
{
    bt_ui_t *ui = (bt_ui_t *)arg;
    ui_cover_msg_t msg = {0};
    while (true) {
        if (xQueueReceive(ui->cover_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ui->media != NULL && msg.data != NULL && msg.size > 0) {
            cover_decoded_t dec = {0};
            bool ok = cover_decode_image(msg.data, msg.size, &dec);
            free(msg.data);
            msg.data = NULL;
            if (ok) {
                lvgl_port_lock(0);
                bt_ui_media_apply_cover(ui->media, &dec);
                lvgl_port_unlock();
            }
        } else if (msg.data != NULL) {
            free(msg.data);
        }
    }
}

#define UI_TAB_SWIPE_MIN_PX  50

static lv_indev_read_cb_t s_touch_read_orig;
static bt_ui_t           *s_tab_ui;
static lv_point_t         s_swipe_start;
static lv_point_t         s_swipe_last;
static bool               s_swipe_pressed;
static int32_t            s_swipe_pending_dx;

static void bt_ui_switch_tab_by_dx(bt_ui_t *ui, int32_t dx)
{
#if LVGL_VERSION_MAJOR >= 9
    uint32_t cur = lv_tabview_get_tab_active(ui->tabview);
    uint32_t cnt = lv_tabview_get_tab_count(ui->tabview);
#else
    uint32_t cur = lv_tabview_get_tab_act(ui->tabview);
    uint32_t cnt = lv_obj_get_child_count(lv_tabview_get_content(ui->tabview));
#endif
    if (cnt == 0u) {
        return;
    }
    uint32_t next = cur;
    /* dx < 0: finger moved left -> next tab (media -> dialer). */
    if (dx < 0) {
        next = (cur + 1u) % cnt;
    } else {
        next = (cur == 0u) ? (cnt - 1u) : (cur - 1u);
    }
    if (next != cur) {
        ESP_LOGI(TAG, "Tab swipe dx=%" PRId32 " %u -> %u", (int32_t)dx, (unsigned)cur, (unsigned)next);
        lv_tabview_set_act(ui->tabview, next, LV_ANIM_OFF);
    }
}

static void bt_ui_tab_swipe_apply_cb(void *unused)
{
    (void)unused;
    bt_ui_t *ui = s_tab_ui;
    int32_t dx = s_swipe_pending_dx;
    s_swipe_pending_dx = 0;
    if (ui == NULL || ui->tabview == NULL || ui->main == NULL) {
        return;
    }
    if (lv_obj_has_flag(ui->main, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    bt_ui_switch_tab_by_dx(ui, dx);
}

static void bt_ui_touch_read_wrapper(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_touch_read_orig != NULL) {
        s_touch_read_orig(indev, data);
    }
    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (!s_swipe_pressed) {
            s_swipe_start = data->point;
            s_swipe_pressed = true;
        }
        s_swipe_last = data->point;
        return;
    }
    if (!s_swipe_pressed) {
        return;
    }
    s_swipe_pressed = false;
    int32_t dx = s_swipe_last.x - s_swipe_start.x;
    int32_t dy = s_swipe_last.y - s_swipe_start.y;
    if (LV_ABS(dx) < UI_TAB_SWIPE_MIN_PX || LV_ABS(dx) <= LV_ABS(dy)) {
        return;
    }
    s_swipe_pending_dx = dx;
    lv_async_call(bt_ui_tab_swipe_apply_cb, NULL);
}

static void bt_ui_bind_tab_swipe(bt_ui_t *ui)
{
    s_tab_ui = ui;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_read_cb_t read_cb = lv_indev_get_read_cb(indev);
            if (read_cb != NULL && read_cb != bt_ui_touch_read_wrapper) {
                s_touch_read_orig = read_cb;
                lv_indev_set_read_cb(indev, bt_ui_touch_read_wrapper);
                ESP_LOGI(TAG, "Tab swipe bound to pointer indev");
            }
        }
        indev = lv_indev_get_next(indev);
    }
    if (s_touch_read_orig == NULL) {
        ESP_LOGW(TAG, "No pointer indev for tab swipe");
    }
}

esp_err_t bt_ui_init(void)
{
    dev_display_lcd_handles_t *lcd_handles = NULL;
    dev_display_lcd_config_t *lcd_cfg = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&lcd_handles);
    if (ret != ESP_OK || lcd_handles == NULL) {
        ESP_LOGE(TAG, "Failed to get LCD handle: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    ret = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&lcd_cfg);
    if (ret != ESP_OK || lcd_cfg == NULL) {
        ESP_LOGE(TAG, "Failed to get LCD config: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = LVGL_TASK_STACK_SIZE;
    lvgl_cfg.task_affinity = LVGL_TASK_AFFINITY;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_handles->io_handle,
        .panel_handle = lcd_handles->panel_handle,
        .buffer_size = lcd_cfg->lcd_width * LVGL_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = lcd_cfg->lcd_width,
        .vres = lcd_cfg->lcd_height,
        .monochrome = false,
        .rotation = {
            .swap_xy = lcd_cfg->swap_xy,
            .mirror_x = lcd_cfg->mirror_x,
            .mirror_y = lcd_cfg->mirror_y,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = (lcd_cfg->bits_per_pixel >= 24) ? LV_COLOR_FORMAT_RGB888
                                                        : LV_COLOR_FORMAT_RGB565,
#endif  /* LVGL_VERSION_MAJOR >= 9 */
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };

    lv_disp_t *disp = NULL;
    if (strcmp(lcd_cfg->sub_type, "rgb") == 0) {
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        const lvgl_port_display_rgb_cfg_t rgb_cfg = {
            .flags = {
                .bb_mode = lcd_cfg->sub_cfg.rgb.panel_config.bounce_buffer_size_px > 0,
                .avoid_tearing = lcd_cfg->sub_cfg.rgb.panel_config.num_fbs > 1,
            },
        };
        disp_cfg.flags.direct_mode = rgb_cfg.flags.avoid_tearing;
        disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
#endif  /* CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT */
    } else {
        disp = lvgl_port_add_disp(&disp_cfg);
    }
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }

#ifdef CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    dev_lcd_touch_handles_t *touch_handles = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, (void **)&touch_handles);
    if (ret == ESP_OK && touch_handles != NULL && touch_handles->touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch_handles->touch_handle,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "Failed to add LVGL touch input");
        }
    } else {
        ESP_LOGW(TAG, "LCD touch unavailable: %s", esp_err_to_name(ret));
    }
#endif  /* CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT */
    return ESP_OK;
}

bt_ui_t *bt_ui_create(const char *device_name, const bt_ui_config_t *config)
{
    bt_ui_t *ui = calloc(1, sizeof(bt_ui_t));
    if (ui == NULL) {
        ESP_LOGE(TAG, "Failed to allocate UI handle");
        return NULL;
    }
    ui->volume = 50;

    /* Cover-art queue */
    ui->cover_queue = xQueueCreate(UI_COVER_QUEUE_SIZE, sizeof(ui_cover_msg_t));
    if (ui->cover_queue == NULL) {
        ESP_LOGW(TAG, "Create UI cover queue failed");
        free(ui);
        return NULL;
    }
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(bt_ui_cover_task, "ui_cover", UI_COVER_TASK_STACK_SIZE,
                                                          ui, UI_COVER_TASK_PRIO, NULL,
                                                          UI_COVER_TASK_CORE_ID,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        vQueueDelete(ui->cover_queue);
        free(ui);
        ESP_LOGW(TAG, "Create UI cover task failed");
        return NULL;
    }

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    ui->splash = bt_ui_splash_create(scr, device_name);

    ui->main = lv_obj_create(scr);
    lv_obj_remove_style_all(ui->main);
    lv_obj_set_size(ui->main, BT_UI_WIDTH, BT_UI_HEIGHT);
    lv_obj_align(ui->main, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(ui->main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui->main, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(ui->main, LV_OBJ_FLAG_HIDDEN);

#if LVGL_VERSION_MAJOR >= 9
    ui->tabview = lv_tabview_create(ui->main);
    lv_tabview_set_tab_bar_size(ui->tabview, 0);
#else
    ui->tabview = lv_tabview_create(ui->main, LV_DIR_TOP, 0);
#endif  /* LVGL_VERSION_MAJOR >= 9 */
    lv_obj_set_pos(ui->tabview, 0, 0);
    lv_obj_set_size(ui->tabview, BT_UI_WIDTH, BT_UI_HEIGHT);

    lv_obj_t *tab_dialer = lv_tabview_add_tab(ui->tabview, "");
    lv_obj_t *tab_music = lv_tabview_add_tab(ui->tabview, "");
    lv_obj_clear_flag(tab_dialer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_music, LV_OBJ_FLAG_SCROLLABLE);

    ui->dialer = bt_ui_dialer_create(tab_dialer,
                                     config ? config->dial_cb : NULL, config ? config->dial_ctx : NULL,
                                     config ? config->end_call_cb : NULL, config ? config->end_call_ctx : NULL,
                                     config ? config->answer_call_cb : NULL, config ? config->answer_call_ctx : NULL);
    ui->media = bt_ui_media_create(tab_music,
                                   config ? config->play_pause_cb : NULL, config ? config->play_pause_ctx : NULL,
                                   config ? config->prev_cb : NULL, config ? config->next_cb : NULL,
                                   config ? config->prev_next_ctx : NULL);
    ui->volume_bar = bt_ui_volume_bar_create(ui->main);
    bt_ui_volume_bar_set_level(ui->volume_bar, ui->volume);

    /* Do not let the user drag the 1600px tab strip (that reboots the RGB
     * bounce ISR), but keep SCROLLABLE so lv_tabview_set_act() can jump. */
    lv_obj_t *tv_content = lv_tabview_get_content(ui->tabview);
    lv_obj_set_scroll_dir(tv_content, LV_DIR_NONE);
    lv_obj_clear_flag(tv_content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    bt_ui_bind_tab_swipe(ui);

    lv_tabview_set_act(ui->tabview, 1, LV_ANIM_OFF);
    lvgl_port_unlock();

    return ui;
}

void bt_ui_set_connected(bt_ui_t *ui, bool connected, esp_bt_audio_tech_t tech)
{
    if (ui == NULL || ui->splash == NULL || ui->main == NULL) {
        return;
    }
    lvgl_port_lock(0);
    if (connected) {
        lv_obj_add_flag(ui->splash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui->main, LV_OBJ_FLAG_HIDDEN);
        bt_ui_media_set_channel_locations(ui->media, bt_ui_channel_locations_from_tech(tech),
                                          tech == ESP_BT_AUDIO_TECH_LE);
    } else {
        lv_obj_clear_flag(ui->splash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->main, LV_OBJ_FLAG_HIDDEN);
        bt_ui_media_set_channel_locations(ui->media, 0, false);
    }
    lvgl_port_unlock();
}

void bt_ui_update_volume(bt_ui_t *ui, int volume)
{
    if (ui == NULL) {
        return;
    }
    ui->volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (ui->volume_bar == NULL) {
        return;
    }
    lvgl_port_lock(0);
    bt_ui_volume_bar_set_level(ui->volume_bar, ui->volume);
    lvgl_port_unlock();
}

int bt_ui_get_volume(const bt_ui_t *ui)
{
    return ui ? ui->volume : 0;
}

void bt_ui_update_playback_status(bt_ui_t *ui, uint32_t play_status)
{
    if (ui == NULL || ui->media == NULL) {
        return;
    }
    lvgl_port_lock(0);
    bt_ui_media_set_playing(ui->media, play_status == ESP_BT_AUDIO_PLAYBACK_STATUS_PLAYING);
    lvgl_port_unlock();
}

void bt_ui_update_track(bt_ui_t *ui, const char *title, const char *artist)
{
    if (ui == NULL || ui->media == NULL) {
        return;
    }
    if (ui->suppress_track_metadata) {
        return;
    }
    lvgl_port_lock(0);
    bt_ui_media_set_track(ui->media, title, artist);
    lvgl_port_unlock();
}

void bt_ui_update_stream_state(bt_ui_t *ui, esp_bt_audio_stream_handle_t stream,
                               esp_bt_audio_stream_state_t state)
{
    if (ui == NULL || ui->media == NULL || stream == NULL) {
        return;
    }
    if (state == ESP_BT_AUDIO_STREAM_STATE_STARTED) {
        esp_bt_audio_stream_profile_t profile = ESP_BT_AUDIO_STREAM_PROFILE_UNKNOWN;
        if (esp_bt_audio_stream_get_profile(stream, &profile) != ESP_OK) {
            return;
        }
        esp_bt_audio_stream_dir_t dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
        if (esp_bt_audio_stream_get_dir(stream, &dir) != ESP_OK) {
            return;
        }
        if (profile != ESP_BT_AUDIO_STREAM_PROFILE_LE_UNICAST &&
            profile != ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST) {
            return;
        }
        bool broadcast_stream = (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST);
        bool broadcast_source = (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST &&
                                 dir == ESP_BT_AUDIO_STREAM_DIR_SOURCE);
        ui->suppress_track_metadata = broadcast_stream;
        ui->stream = stream;
        lvgl_port_lock(0);
        if (broadcast_source) {
            if (ui->splash != NULL) {
                lv_obj_add_flag(ui->splash, LV_OBJ_FLAG_HIDDEN);
            }
            if (ui->main != NULL) {
                lv_obj_clear_flag(ui->main, LV_OBJ_FLAG_HIDDEN);
            }
            if (ui->tabview != NULL) {
                lv_tabview_set_act(ui->tabview, 1, LV_ANIM_OFF);
            }
        }
        bt_ui_media_set_stream_type(ui->media, profile);
        if (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST) {
            bt_ui_media_set_track(ui->media, broadcast_source ? "Broadcasting audio" : "Receiving broadcast", "");
        }
        lvgl_port_unlock();
    } else if ((state == ESP_BT_AUDIO_STREAM_STATE_STOPPED || state == ESP_BT_AUDIO_STREAM_STATE_RELEASED) &&
               ui->stream == stream) {
        esp_bt_audio_stream_profile_t profile = ESP_BT_AUDIO_STREAM_PROFILE_UNKNOWN;
        esp_bt_audio_stream_dir_t dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
        if (esp_bt_audio_stream_get_profile(stream, &profile) != ESP_OK) {
            profile = ESP_BT_AUDIO_STREAM_PROFILE_UNKNOWN;
        }
        if (esp_bt_audio_stream_get_dir(stream, &dir) != ESP_OK) {
            dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
        }
        bool clear_broadcast_title = (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST);
        bool restore_broadcast_source_ui = (profile == ESP_BT_AUDIO_STREAM_PROFILE_LE_BROADCAST &&
                                            dir == ESP_BT_AUDIO_STREAM_DIR_SOURCE);
        ui->stream = NULL;
        ui->suppress_track_metadata = false;
        lvgl_port_lock(0);
        bt_ui_media_set_stream_type(ui->media, ESP_BT_AUDIO_STREAM_PROFILE_UNKNOWN);
        if (clear_broadcast_title) {
            bt_ui_media_set_track(ui->media, "", "");
        }
        if (restore_broadcast_source_ui) {
            if (ui->splash != NULL) {
                lv_obj_clear_flag(ui->splash, LV_OBJ_FLAG_HIDDEN);
            }
            if (ui->main != NULL) {
                lv_obj_add_flag(ui->main, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lvgl_port_unlock();
    }
}

void bt_ui_update_call_state(bt_ui_t *ui, int state, const char *number)
{
    if (ui == NULL || ui->dialer == NULL) {
        return;
    }
    lvgl_port_lock(0);
    bt_ui_dialer_set_call_state(ui->dialer, state, number);
    /* Switch to the dialer tab on any call activity; return to media when idle */
    if (ui->tabview != NULL) {
        lv_tabview_set_act(ui->tabview, state != 0 ? 0 : 1, LV_ANIM_OFF);
    }
    lvgl_port_unlock();
}

void bt_ui_post_cover(bt_ui_t *ui, const uint8_t *data, size_t size)
{
    if (ui == NULL || ui->cover_queue == NULL || data == NULL || size == 0) {
        return;
    }
    uint8_t *copy = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    if (copy == NULL) {
        ESP_LOGW(TAG, "No memory for cover art copy, size %u", (unsigned)size);
        return;
    }
    memcpy(copy, data, size);
    ui_cover_msg_t msg = {
        .data = copy,
        .size = size,
    };
    if (xQueueSend(ui->cover_queue, &msg, 0) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "Cover art UI queue full, drop update");
        return;
    }
    ESP_LOGI(TAG, "Cover art queued, %u bytes", (unsigned)size);
}
