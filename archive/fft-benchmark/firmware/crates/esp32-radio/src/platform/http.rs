//! Blocking certificate-verified HTTPS, owned exclusively by signaling.
use super::ffi;
use crate::error::{Error, Result};
use std::{
    ffi::{CStr, c_void},
    marker::PhantomData,
    ptr::NonNull,
    rc::Rc,
    sync::atomic::{AtomicBool, Ordering},
};

static HTTP_TAKEN: AtomicBool = AtomicBool::new(false);
pub(crate) const RESPONSE_LIMIT: usize = 24_576;

#[derive(Debug)]
pub(crate) struct Http {
    handle: NonNull<c_void>,
    _task: PhantomData<Rc<()>>,
}

impl Http {
    pub(crate) fn open() -> Result<Self> {
        if HTTP_TAKEN.swap(true, Ordering::AcqRel) {
            return Err(Error::new("HTTP task already initialized"));
        }
        loop {
            // SAFETY: only the successful guard owner may initialize/synchronize
            // SNTP. C owns all configuration; no Rust callback or borrowed memory.
            match unsafe { ffi::radio_clock_sync() } {
                1 => break,
                -1 => return Err(Error::new("clock initialization failed")),
                _ => super::log("Waiting for time synchronization before verified HTTPS"),
            }
        }
        // SAFETY: C returns a fresh client owning all callback buffers. It uses
        // compiled credentials internally; no secret enters Rust diagnostics.
        let handle = NonNull::new(unsafe { ffi::radio_http_open() })
            .ok_or(Error::new("HTTPS initialization failed"))?;
        Ok(Self {
            handle,
            _task: PhantomData,
        })
    }
    pub(crate) fn post(
        &mut self,
        path: &CStr,
        body: &[u8],
        out: &mut [u8],
    ) -> Result<(i32, usize)> {
        let mut used = 0;
        // SAFETY: all pointers are live for this blocking call; output is uniquely
        // borrowed and disjoint from body. C clears its borrowed POST field before
        // return. Events use C-owned response storage, then copy within capacity.
        let status = unsafe {
            ffi::radio_http_post(
                self.handle.as_ptr(),
                path.as_ptr(),
                body.as_ptr(),
                body.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut used,
            )
        };
        if used > out.len() {
            return Err(Error::new("invalid HTTPS response length"));
        }
        Ok((status, used))
    }
}
impl Drop for Http {
    fn drop(&mut self) {
        // SAFETY: all blocking calls/callbacks have returned; same task and sole
        // owner. C cleans up the client before freeing its callback context.
        unsafe { ffi::radio_http_free(self.handle.as_ptr()) };
    }
}
