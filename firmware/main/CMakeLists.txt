idf_component_register(SRC_DIRS "." "fonts"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_http_client esp_http_server esp-tls esp_https_ota esp_netif
                                app_update nvs_flash json mbedtls esp_driver_gpio esp_driver_ledc
                                esp_driver_i2c esp_driver_spi esp_lcd esp_timer lwip esp_app_format)
