//! Task ownership and terminal recovery. All ordinary state is explicitly owned.
use crate::{
    console,
    error::{Error, Result},
    platform::{self, Board},
    radio, signaling,
};
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::sync_channel,
    },
    thread,
    time::Duration,
};

pub(crate) fn run() -> Result<()> {
    platform::log("Pocket Radio: Rust application starting");
    let board = Board::init()?;
    let (requests, radio_requests) = sync_channel(4);
    let (radio_events, events) = sync_channel(8);
    thread::Builder::new()
        .name("radio".into())
        .stack_size(16_384)
        .spawn(move || {
            if let Err(error) = radio::run(board, radio_requests, radio_events) {
                recover(error);
            }
        })
        .map_err(|_| Error::new("cannot start radio task"))?;
    thread::Builder::new()
        .name("signaling".into())
        .stack_size(16_384)
        .spawn(move || {
            if let Err(error) = signaling::run(requests, events) {
                recover(error);
            }
        })
        .map_err(|_| Error::new("cannot start signaling task"))?;
    console::run()
}

static RECOVERING: AtomicBool = AtomicBool::new(false);

/// Fatal runtime failures restart the complete native transport. One caller owns
/// the recovery counter; concurrent failures park until reset, avoiding races.
pub(crate) fn recover(error: Error) -> ! {
    if RECOVERING.swap(true, Ordering::AcqRel) {
        loop {
            thread::sleep(Duration::from_secs(1));
        }
    }
    let seconds = 5u64 << platform::recovery_attempt(false).min(3);
    platform::log(&format!("{error}; restarting in {seconds} seconds"));
    thread::sleep(Duration::from_millis(
        seconds * 1000 + u64::from(platform::random() % 1000),
    ));
    platform::restart()
}
