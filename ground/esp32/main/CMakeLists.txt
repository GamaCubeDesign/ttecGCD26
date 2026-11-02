idf_component_register(
    SRCS "gs_main.c" "radio_esp32.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ttec esp_driver_spi esp_driver_gpio esp_driver_uart esp_timer
                  esp_app_format esp_system)
