idf_component_register(
    SRCS "observer.c" "ota.c" "router.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_event esp-tls esp_timer esp_netif esp_wifi
             mbedtls nvs_flash esp_http_server app_update
)
