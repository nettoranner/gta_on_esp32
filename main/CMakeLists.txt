idf_component_register(
    SRCS "main.c" "game.c" "input_local.c" "bt_gamepad.c" "hid_gamepad_parse.c" "storage.c" "esp_hid_gap.c" "pad_map.c" "usb_gamepad.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_adc esp_timer nvs_flash bt esp_hid fatfs sdmmc esp_driver_sdspi usb
)
# -O2 for the game core (rendering)
set_source_files_properties(game.c PROPERTIES COMPILE_OPTIONS "-O2")
