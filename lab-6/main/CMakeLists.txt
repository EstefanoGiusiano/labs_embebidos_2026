idf_component_register(SRCS "nodo_mqtt_main.c" "red_wifi.c" "microfono.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                esp_driver_tsens esp_timer esp_driver_i2c
                                esp_driver_i2s mbedtls mqtt led_strip
                                esp_codec_dev)
