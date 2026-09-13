//! Hand-written declarations for our small private C ABI, not vendor layouts.
//! Keep signatures in sync with platform/include/radio_bridge.h.
use std::ffi::{c_char, c_void};

unsafe extern "C" {
    pub(super) fn radio_board_init() -> i32;
    pub(super) fn radio_clock_sync() -> i32;
    pub(super) fn radio_now_us() -> u64;
    pub(super) fn radio_random() -> u32;
    pub(super) fn radio_heap_free() -> u32;
    pub(super) fn radio_stack_free() -> u32;
    pub(super) fn radio_rssi() -> i32;
    pub(super) fn radio_led(r: u8, g: u8, b: u8) -> i32;
    pub(super) fn radio_console_byte() -> i32;
    pub(super) fn radio_log(message: *const c_char);
    pub(super) fn radio_recovery_attempt(reset: i32) -> u32;
    pub(super) fn radio_restart() -> !;
    pub(super) fn radio_music_load(length: *mut usize) -> *mut u8;
    pub(super) fn radio_music_free(bytes: *mut u8);
    pub(super) fn radio_peer_open() -> *mut c_void;
    pub(super) fn radio_peer_poll(peer: *mut c_void) -> i32;
    pub(super) fn radio_peer_offer(peer: *mut c_void, out: *mut u8, capacity: usize) -> i32;
    pub(super) fn radio_peer_answer(peer: *mut c_void, bytes: *const u8, length: usize) -> i32;
    pub(super) fn radio_peer_state(peer: *mut c_void) -> i32;
    pub(super) fn radio_peer_channels(peer: *mut c_void) -> i32;
    pub(super) fn radio_peer_command(peer: *mut c_void, out: *mut u8, capacity: usize) -> i32;
    pub(super) fn radio_peer_dropped(peer: *mut c_void) -> u32;
    pub(super) fn radio_peer_audio(
        peer: *mut c_void,
        pts: u32,
        bytes: *const u8,
        length: usize,
    ) -> i32;
    pub(super) fn radio_peer_data(
        peer: *mut c_void,
        stream: u16,
        binary: i32,
        bytes: *const u8,
        length: usize,
    ) -> i32;
    pub(super) fn radio_http_open() -> *mut c_void;
    pub(super) fn radio_http_post(
        http: *mut c_void,
        path: *const c_char,
        body: *const u8,
        length: usize,
        out: *mut u8,
        capacity: usize,
        used: *mut usize,
    ) -> i32;
    pub(super) fn radio_http_free(http: *mut c_void);
}
