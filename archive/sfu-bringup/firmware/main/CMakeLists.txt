idf_component_register(SRCS "hardware_check.c" "peer_probe.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/private"
    PRIV_REQUIRES esp_psram esp_driver_gpio esp_wifi esp_netif esp_event
        esp_http_client esp-tls esp_timer esp_system spi_flash heap json esp_peer)
