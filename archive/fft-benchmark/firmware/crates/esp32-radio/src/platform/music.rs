//! C allocates/frees PSRAM; Rust only borrows its immutable initialized contents.
use super::ffi;
use crate::error::{Error, Result};
use std::{ptr::NonNull, slice};

#[derive(Debug)]
pub(crate) struct MusicBytes {
    pointer: NonNull<u8>,
    length: usize,
}

impl MusicBytes {
    pub(crate) fn load() -> Result<Self> {
        let mut length = 0;
        // SAFETY: length is writable for the call. On success C transfers a
        // fresh, fully initialized PSRAM allocation with no remaining aliases.
        let pointer = NonNull::new(unsafe { ffi::radio_music_load(&mut length) })
            .ok_or(Error::new("music partition could not be loaded"))?;
        let bytes = Self { pointer, length };
        if length != 4 * 1024 * 1024 {
            return Err(Error::new("unexpected music partition size"));
        }
        Ok(bytes)
    }
    pub(crate) fn as_slice(&self) -> &[u8] {
        // SAFETY: C returned exactly length initialized bytes in one allocation,
        // bounded to 4 MiB (< isize::MAX). No C aliases remain. The returned
        // lifetime is tied to self, so Drop cannot free live Rust borrows.
        unsafe { slice::from_raw_parts(self.pointer.as_ptr(), self.length) }
    }
}
impl Drop for MusicBytes {
    fn drop(&mut self) {
        // SAFETY: sole allocation owner; borrows have ended; free on C's allocator.
        unsafe { ffi::radio_music_free(self.pointer.as_ptr()) };
    }
}
