idf_component_register(SRCS "espnow_example_main.c" "nvs_helper.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi
                    REQUIRES esp_driver_usb_serial_jtag json
                    )
