//! Safe application-facing boundary around ESP-IDF. Native handles never escape.
//! There are no Rust callbacks from C and no unsafe Send/Sync implementations.
mod ffi;
mod http;
mod music;
mod peer;

use crate::error::{Error, Result, check};
pub(crate) use http::{Http, RESPONSE_LIMIT};
pub(crate) use music::MusicBytes;
pub(crate) use peer::{Peer, PeerState};
use radio_core::protocol::Color;
use std::{
    cell::Cell,
    ffi::CString,
    marker::PhantomData,
    sync::atomic::{AtomicBool, Ordering},
};

static BOARD_TAKEN: AtomicBool = AtomicBool::new(false);

/// Unique LED owner; moved to the radio thread after initialization, never shared.
#[derive(Debug)]
pub(crate) struct Board {
    _not_sync: PhantomData<Cell<()>>,
}

impl Board {
    pub(crate) fn init() -> Result<Self> {
        if BOARD_TAKEN.swap(true, Ordering::AcqRel) {
            return Err(Error::new("board already initialized"));
        }
        // SAFETY: the atomic guard permits exactly one initialization; C owns all
        // driver state. It registers only C callbacks and borrows no Rust memory.
        let code = unsafe { ffi::radio_board_init() };
        check(code, "board initialization failed")?;
        Ok(Self {
            _not_sync: PhantomData,
        })
    }
    pub(crate) fn set_led(&mut self, Color([r, g, b]): Color) -> Result<()> {
        // SAFETY: a Board exists only after initialization; &mut self serializes
        // LED calls and this sole owner is not Sync. Components fit the C ABI.
        check(unsafe { ffi::radio_led(r, g, b) }, "LED write failed")
    }
}

pub(crate) fn now_us() -> u64 {
    // SAFETY: thread-safe IDF monotonic timer; no borrowed state or pointers.
    unsafe { ffi::radio_now_us() }
}
pub(crate) fn random() -> u32 {
    // SAFETY: thread-safe IDF entropy API; Wi-Fi is active for the application.
    unsafe { ffi::radio_random() }
}
pub(crate) fn heap_free() -> u32 {
    // SAFETY: IDF serializes heap inspection internally; scalar result only.
    unsafe { ffi::radio_heap_free() }
}
pub(crate) fn stack_free() -> u32 {
    // SAFETY: query of the calling FreeRTOS task, with no external pointers.
    unsafe { ffi::radio_stack_free() }
}
pub(crate) fn rssi() -> i32 {
    // SAFETY: IDF Wi-Fi API synchronizes access and C initializes its output.
    unsafe { ffi::radio_rssi() }
}
pub(crate) fn console_byte() -> i32 {
    // SAFETY: called only by app_main's console loop; C stdio owns its buffer.
    unsafe { ffi::radio_console_byte() }
}
pub(crate) fn log(message: &str) {
    if let Ok(message) = CString::new(message) {
        // SAFETY: valid terminated string remains live until synchronous logging
        // returns. C uses "%s", so text cannot become a format string.
        unsafe { ffi::radio_log(message.as_ptr()) };
    }
}
pub(crate) fn recovery_attempt(reset: bool) -> u32 {
    // SAFETY: called only by signaling (or fatal recovery which never returns).
    // C retains the bounded scalar counter across software resets.
    unsafe { ffi::radio_recovery_attempt(i32::from(reset)) }
}
pub(crate) fn restart() -> ! {
    // SAFETY: IDF reset does not return or unwind and takes no borrowed memory.
    unsafe { ffi::radio_restart() }
}
