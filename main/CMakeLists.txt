set(ui_srcs "")
if(CONFIG_EXAMPLE_BT_UI_ENABLE)
    list(APPEND ui_srcs "bt_ui/bt_ui.c"
         "bt_ui/images/cis_stream_icon.c"
         "bt_ui/images/bis_stream_icon.c"
         "bt_ui/fonts/lv_font_notosanssc_regular_28.c")
endif()

# esp_bt_audio is a local component (components/esp_bt_audio) rather than a
# managed one, so its headers are only visible if main names it explicitly -
# the dependency no longer flows in through idf_component.yml.
set(reqs "bt" "nvs_flash" "console" "esp_board_manager" "esp_codec_dev" "esp_bt_audio")
if(CONFIG_EXAMPLE_BT_UI_ENABLE)
    list(APPEND reqs "esp_lvgl_port" "lvgl" "esp_jpeg")
endif()

idf_component_register(SRCS "main.c" "pool_reg.c" "cmd_reg.c" "stream_proc.c"
                    ${ui_srcs}
                    INCLUDE_DIRS "." "bt_ui"
                    REQUIRES ${reqs}
                    PRIV_REQUIRES esp_driver_jpeg
                    )

# Opus (silk tables) and LC3 (FFT/MDCT tables) are each vendored twice with identical
# content: once in the ported in-stack decoders inside libbt.a, once in
# esp_audio_codec (pulled in transitively by esp_bt_audio / gmf_audio). Both copies are
# read-only constants of the same upstream revisions, so whichever the linker keeps is
# equivalent; -fvisibility=hidden does not demote them because GCC leaves initialized
# const data as GLOBAL unless a version script says otherwise.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
