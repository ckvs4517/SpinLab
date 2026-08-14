idf_component_register(SRCS "spinlab_main.c" "spinlab_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_timer)
