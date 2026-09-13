//! Checked reader for the version 1 pack produced by `prepare-music.py`.
use std::{fmt, num::NonZeroU32};

pub const FRAME_MS: u32 = 20;
pub const BANDS: usize = 32;
pub const MAX_OPUS_BYTES: usize = 1275;
const HEADER_BYTES: usize = 64;
const MAX_FRAMES: u32 = 30_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Header,
    Format,
    FrameCount,
    Truncated,
    PacketSize,
    Checksum,
    Memory,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Header => "invalid music pack header",
            Self::Format => "music must be stereo 48 kHz Opus with 20 ms frames",
            Self::FrameCount => "music frame count is outside the supported range",
            Self::Truncated => "music pack ends inside a frame",
            Self::PacketSize => "invalid Opus packet size",
            Self::Checksum => "music pack checksum mismatch",
            Self::Memory => "cannot allocate music frame index",
        })
    }
}
impl std::error::Error for Error {}

/// A validated pack borrowing immutable storage, including any erased flash tail.
#[derive(Debug)]
pub struct Pack<'a> {
    bytes: &'a [u8],
    offsets: Vec<usize>,
    frames: NonZeroU32,
}

#[derive(Debug, Clone, Copy)]
pub struct Frame<'a> {
    pub opus: &'a [u8],
    pub bands: &'a [u8; BANDS],
}

impl<'a> Pack<'a> {
    /// Checks every record boundary and the CRC before exposing any frame.
    pub fn parse(bytes: &'a [u8]) -> Result<Self, Error> {
        if bytes.len() < HEADER_BYTES || &bytes[..8] != b"S3MUSIC\0" || le32(bytes, 8) != 1 {
            return Err(Error::Header);
        }
        if le32(bytes, 16) != 48_000 || le32(bytes, 20) != 2 || le32(bytes, 24) != FRAME_MS {
            return Err(Error::Format);
        }
        let frames = NonZeroU32::new(le32(bytes, 12))
            .filter(|v| v.get() <= MAX_FRAMES)
            .ok_or(Error::FrameCount)?;
        let mut offsets = Vec::new();
        offsets
            .try_reserve_exact(frames.get() as usize)
            .map_err(|_| Error::Memory)?;
        let mut cursor = HEADER_BYTES;
        for _ in 0..frames.get() {
            let record = bytes
                .get(cursor..cursor + 2 + BANDS)
                .ok_or(Error::Truncated)?;
            let size = u16::from_le_bytes([record[0], record[1]]) as usize;
            if !(1..=MAX_OPUS_BYTES).contains(&size) {
                return Err(Error::PacketSize);
            }
            let end = cursor + 2 + BANDS + size;
            if end > bytes.len() {
                return Err(Error::Truncated);
            }
            offsets.push(cursor);
            cursor = end;
        }
        if crc32fast::hash(&bytes[HEADER_BYTES..cursor]) != le32(bytes, 28) {
            return Err(Error::Checksum);
        }
        Ok(Self {
            bytes: &bytes[..cursor],
            offsets,
            frames,
        })
    }

    pub fn frame_count(&self) -> NonZeroU32 {
        self.frames
    }

    pub fn frame(&self, index: u32) -> Option<Frame<'a>> {
        let start = *self.offsets.get(index as usize)?;
        let size = u16::from_le_bytes([self.bytes[start], self.bytes[start + 1]]) as usize;
        Some(Frame {
            opus: &self.bytes[start + 2 + BANDS..start + 2 + BANDS + size],
            bands: self.bytes[start + 2..start + 2 + BANDS].try_into().ok()?,
        })
    }
}

// Only called after validating the complete fixed-size header.
fn le32(bytes: &[u8], start: usize) -> u32 {
    u32::from_le_bytes(
        bytes[start..start + 4]
            .try_into()
            .expect("validated header"),
    )
}
