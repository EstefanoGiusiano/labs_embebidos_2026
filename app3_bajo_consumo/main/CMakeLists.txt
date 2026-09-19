idf_component_register(SRCS "app3_bajo_consumo_main.c"
                       INCLUDE_DIRS ""
                       REQUIRES esp_driver_touch_sens esp_driver_uart esp_timer
                                esp_partition)
