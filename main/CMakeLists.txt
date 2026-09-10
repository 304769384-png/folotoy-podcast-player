idf_component_register(
    SRCS "main.cc"
         "podcast_catalog.cc"
         "podcast_feed_parser.cc"
         "podcast_player.cc"
         "podcast_ui.cc"
         "radio_display_settings.cc"
         "radio_sleep_timer.cc"
         "pod_font.c" "pod_font_cjk.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp
        nvs_flash
        esp_http_client
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_lcd
        esp_partition
        esp_rom
        spi_flash
        mbedtls
        esp_audio_codec
        esp_driver_usb_serial_jtag
        esp-wifi-connect
)
