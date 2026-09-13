idf_component_register(SRCS "main.c" "radio.c" "signaling.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/private"
    PRIV_REQUIRES esp_psram esp_driver_gpio esp_wifi esp_netif esp_event
        esp_timer esp_system spi_flash esp_partition heap json esp_peer
        esp_http_client esp-tls pthread vfs)
