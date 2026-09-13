//! Task-affine peer. Native callbacks terminate in C-owned bounded queues.
use super::ffi;
use crate::error::{Error, Result, check};
use std::{ffi::c_void, marker::PhantomData, ptr::NonNull, rc::Rc};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum PeerState {
    Connecting,
    Connected,
    Lost,
}

/// C owns this single peer until reboot. Intentionally no close-on-Drop: vendor
/// teardown currently hangs. The private raw handle and Rc marker forbid sharing
/// or moving it to another task; no Rust allocation is retained by its callbacks.
#[derive(Debug)]
pub(crate) struct Peer {
    handle: NonNull<c_void>,
    _task: PhantomData<Rc<()>>,
}

impl Peer {
    pub(crate) fn open() -> Result<Self> {
        // SAFETY: C enforces single creation, owns all callback contexts and
        // configuration until reboot, and returns either null or a live handle.
        let handle = NonNull::new(unsafe { ffi::radio_peer_open() })
            .ok_or(Error::new("peer initialization failed"))?;
        Ok(Self {
            handle,
            _task: PhantomData,
        })
    }
    pub(crate) fn poll(&mut self) {
        // SAFETY: private live handle, same task; callbacks access C queues only.
        unsafe { ffi::radio_peer_poll(self.handle.as_ptr()) };
    }
    pub(crate) fn state(&self) -> PeerState {
        // SAFETY: C performs an atomic read on its live callback state.
        match unsafe { ffi::radio_peer_state(self.handle.as_ptr()) } {
            1 => PeerState::Connected,
            -1 => PeerState::Lost,
            _ => PeerState::Connecting,
        }
    }
    pub(crate) fn offer(&mut self, out: &mut [u8]) -> Result<usize> {
        // SAFETY: C copies at most out.len() bytes while holding its offer lock;
        // it retains no pointer to out. The handle remains on its creating task.
        let length =
            unsafe { ffi::radio_peer_offer(self.handle.as_ptr(), out.as_mut_ptr(), out.len()) };
        bounded_length(length, out.len())
    }
    pub(crate) fn answer(&mut self, sdp: &str) -> Result<()> {
        check(
            // SAFETY: C copies the bytes into its persistent answer storage before
            // invoking the vendor. It rejects oversized and repeated answers.
            unsafe { ffi::radio_peer_answer(self.handle.as_ptr(), sdp.as_ptr(), sdp.len()) },
            "peer rejected answer",
        )
    }
    pub(crate) fn create_channels(&mut self) -> Result<()> {
        check(
            // SAFETY: sole task owner, C validates state and stores configs for life.
            unsafe { ffi::radio_peer_channels(self.handle.as_ptr()) },
            "channel creation failed",
        )
    }
    pub(crate) fn command(&mut self, out: &mut [u8]) -> Result<usize> {
        // SAFETY: C copies a single bounded queue item into caller-owned output
        // and never retains out. Callback and consumer synchronize via the queue.
        let length =
            unsafe { ffi::radio_peer_command(self.handle.as_ptr(), out.as_mut_ptr(), out.len()) };
        bounded_length(length, out.len())
    }
    pub(crate) fn dropped(&self) -> u32 {
        // SAFETY: atomic read from a live, process-lifetime C context.
        unsafe { ffi::radio_peer_dropped(self.handle.as_ptr()) }
    }
    pub(crate) fn audio(&mut self, pts: u32, bytes: &[u8]) -> Result<()> {
        check(
            // SAFETY: the shim bounds-checks and copies input to C-owned storage.
            // No vendor code receives a pointer into this Rust slice; see FFI contract
            // for the pinned encoder's synchronous packet/cache copy behavior.
            unsafe {
                ffi::radio_peer_audio(self.handle.as_ptr(), pts, bytes.as_ptr(), bytes.len())
            },
            "audio send failed",
        )
    }
    pub(crate) fn data(&mut self, stream: u16, binary: bool, bytes: &[u8]) -> Result<()> {
        check(
            // SAFETY: the shim validates stream/length and copies to C-owned storage;
            // SCTP copies into its bounded cache before returning. No Rust pointer is retained.
            unsafe {
                ffi::radio_peer_data(
                    self.handle.as_ptr(),
                    stream,
                    i32::from(binary),
                    bytes.as_ptr(),
                    bytes.len(),
                )
            },
            "data send failed",
        )
    }
}

fn bounded_length(length: i32, capacity: usize) -> Result<usize> {
    let length = usize::try_from(length).map_err(|_| Error::new("invalid peer response length"))?;
    if length > capacity {
        return Err(Error::new("oversized peer response"));
    }
    Ok(length)
}
