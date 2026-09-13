idf_component_register(SRCS "../platform/entry.c" "../platform/board.c" "../platform/peer.c"
    "../platform/http.c" "../platform/spectrum_benchmark.c" WHOLE_ARCHIVE
    INCLUDE_DIRS "../platform/include" "../platform" "${CMAKE_BINARY_DIR}/private"
    PRIV_REQUIRES esp_psram esp_driver_gpio esp_wifi esp_netif esp_event
        esp_timer esp_system spi_flash esp_partition heap json esp_peer esp_http_client esp-tls pthread vfs esp-dsp esp_audio_codec)

set(RUST_TARGET "xtensa-esp32s3-espidf")
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")
set(RUST_ARCHIVE "${RUST_TARGET_DIR}/${RUST_TARGET}/release/libesp32_radio.a")
add_custom_target(radio_rust_build
    COMMAND ${CMAKE_COMMAND} -E env
        "RUSTFLAGS=--cfg espidf_time64"
        "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
        cargo +esp-radio build --manifest-path "${CMAKE_CURRENT_LIST_DIR}/../Cargo.toml"
            --package esp32-radio --release --locked --target ${RUST_TARGET}
            -Zbuild-std=std,panic_abort
    BYPRODUCTS "${RUST_ARCHIVE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    VERBATIM USES_TERMINAL)
add_prebuilt_library(radio_rust "${RUST_ARCHIVE}" PRIV_REQUIRES pthread vfs esp-dsp esp_audio_codec)
add_dependencies(radio_rust radio_rust_build)
target_link_libraries(${COMPONENT_LIB} PRIVATE radio_rust)
