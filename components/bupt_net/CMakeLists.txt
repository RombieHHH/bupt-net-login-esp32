idf_component_register(
    SRCS "bupt_net.c" "bupt_ipv6.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        esp_driver_uart
        lwip
        nvs_flash
        nvs_sec_provider
)
