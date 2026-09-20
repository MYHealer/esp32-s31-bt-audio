/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"

#if __has_include(<esp_lcd_touch_gt911.h>)
#define HAS_GT911  1
#include "esp_lcd_touch_gt911.h"
#endif  /* __has_include(<esp_lcd_touch_gt911.h>) */

static const char *TAG = "S31_ES8389_SETUP_DEVICE";

#if defined(HAS_GT911)
esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GT911 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
#endif  /* defined(HAS_GT911) */
