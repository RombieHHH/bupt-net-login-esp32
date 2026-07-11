idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        esp_event
                        esp_http_client
                        esp_netif
                        esp_wifi
                        esp_driver_uart
                        lwip
                        nvs_flash
                        nvs_sec_provider)
