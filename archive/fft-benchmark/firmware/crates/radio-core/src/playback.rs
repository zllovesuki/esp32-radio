//! Absolute playback deadlines. Late packets are skipped, never sent in a burst.
use crate::{music::FRAME_MS, protocol::Action};
use std::num::NonZeroU32;

const FRAME_US: u64 = FRAME_MS as u64 * 1000;

#[derive(Debug)]
pub struct Playback {
    frames: NonZeroU32,
    index: u32,
    elapsed: u64,
    next_us: u64,
    paused: bool,
    skipped: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Due {
    pub index: u32,
    pub pts_ms: u32,
    pub paused: bool,
    pub spectrum: bool,
}

impl Playback {
    pub fn new(frames: NonZeroU32, now_us: u64) -> Self {
        Self {
            frames,
            index: 0,
            elapsed: 0,
            next_us: now_us,
            paused: false,
            skipped: 0,
        }
    }

    /// Returns at most one packet per call; `now_us` comes from a monotonic clock.
    pub fn due(&mut self, now_us: u64) -> Option<Due> {
        if now_us < self.next_us {
            return None;
        }
        let late = (now_us - self.next_us) / FRAME_US;
        self.elapsed = self.elapsed.wrapping_add(late);
        self.skipped = self.skipped.saturating_add(late);
        if !self.paused {
            self.advance(late);
        }
        self.next_us = self
            .next_us
            .saturating_add((late + 1).saturating_mul(FRAME_US));
        let due = Due {
            index: self.index,
            pts_ms: self.elapsed.wrapping_mul(FRAME_MS as u64) as u32,
            paused: self.paused,
            spectrum: self.elapsed % 2 == 0,
        };
        self.elapsed = self.elapsed.wrapping_add(1);
        if !self.paused {
            self.advance(1);
        }
        Some(due)
    }

    fn advance(&mut self, count: u64) {
        self.index = ((self.index as u64 + count % self.frames.get() as u64)
            % self.frames.get() as u64) as u32;
    }

    pub fn apply(&mut self, action: Action) {
        match action {
            Action::Play => self.paused = false,
            Action::Pause => self.paused = true,
            Action::Restart => {
                self.index = 0;
                self.paused = false;
            }
        }
    }
    pub fn paused(&self) -> bool {
        self.paused
    }
    pub fn position_ms(&self) -> u32 {
        self.index * FRAME_MS
    }
    pub fn duration_ms(&self) -> u32 {
        self.frames.get() * FRAME_MS
    }
    pub fn skipped_frames(&self) -> u64 {
        self.skipped
    }
}
