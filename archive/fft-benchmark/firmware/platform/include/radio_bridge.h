#pragma once
#include <stddef.h>
#include <stdint.h>

/* Private ABI, implemented in C and imported only by esp32-radio::platform.
 * Call with valid pointer/length pairs; no Rust values cross this boundary.
 * All input bytes are borrowed only for the call. Output storage is caller-owned.
 * Peer/HTTP handles stay on their creating task. See docs/ffi.md for lifetimes.
 */
int32_t radio_board_init(void);
int32_t radio_network_ready(void);
int32_t radio_clock_sync(void);
uint64_t radio_now_us(void);
uint32_t radio_random(void);
uint32_t radio_heap_free(void);
uint32_t radio_stack_free(void);
int32_t radio_rssi(void);
int32_t radio_led(uint8_t r, uint8_t g, uint8_t b);
int32_t radio_console_byte(void);
void radio_log(const char *message);
uint32_t radio_recovery_attempt(int32_t reset);
void radio_restart(void) __attribute__((noreturn));
uint8_t *radio_music_load(size_t *length);
void radio_music_free(uint8_t *bytes);

typedef struct radio_peer radio_peer;
radio_peer *radio_peer_open(void);
int32_t radio_peer_poll(radio_peer *peer);
int32_t radio_peer_offer(radio_peer *peer, uint8_t *out, size_t capacity);
int32_t radio_peer_answer(radio_peer *peer, const uint8_t *bytes, size_t length);
/* State is normalized: 0 connecting, 1 SCTP connected, -1 fatal/lost. */
int32_t radio_peer_state(radio_peer *peer);
int32_t radio_peer_channels(radio_peer *peer);
int32_t radio_peer_command(radio_peer *peer, uint8_t *out, size_t capacity);
uint32_t radio_peer_dropped(radio_peer *peer);
int32_t radio_peer_audio(radio_peer *peer, uint32_t pts, const uint8_t *bytes, size_t length);
int32_t radio_peer_data(radio_peer *peer, uint16_t stream, int32_t binary, const uint8_t *bytes, size_t length);

typedef struct radio_http radio_http;
radio_http *radio_http_open(void);
int32_t radio_http_post(radio_http *http, const char *path, const uint8_t *body, size_t length,
                       uint8_t *out, size_t capacity, size_t *used);
void radio_http_free(radio_http *http);

/* Called exactly once by app_main. The Rust release profile aborts on panic. */
void radio_rust_main(void);
