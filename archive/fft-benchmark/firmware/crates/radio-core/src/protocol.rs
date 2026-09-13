//! Browser wire contracts. Maintenance commands are deliberately a separate API.
use crate::{
    music::{BANDS, FRAME_MS},
    playback::Due,
};
use serde::{Deserialize, Serialize};

pub const COMMAND_LIMIT: usize = 512;
pub const ROBOT_ID: u16 = 2;
pub const SPECTRUM_ID: u16 = 4;
/// RFC 6716 silence packet; keep the RTP clock running during pause.
pub const SILENCE: &[u8] = &[0xf8, 0xff, 0xfe];

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Action {
    Play,
    Pause,
    Restart,
}

/// Deserialization enforces exactly three integer color components in 0..=255.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(transparent)]
pub struct Color(pub [u8; 3]);

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Command {
    pub led: Option<Color>,
    pub action: Option<Action>,
    pub command_id: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CommandError {
    TooLarge,
    Invalid,
    Empty,
    IdTooLong,
}

impl std::fmt::Display for CommandError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "invalid control command: {self:?}")
    }
}
impl std::error::Error for CommandError {}

impl Command {
    pub fn parse(bytes: &[u8]) -> Result<Self, CommandError> {
        if bytes.len() > COMMAND_LIMIT {
            return Err(CommandError::TooLarge);
        }
        let command: Self = serde_json::from_slice(bytes).map_err(|_| CommandError::Invalid)?;
        if command.led.is_none() && command.action.is_none() {
            return Err(CommandError::Empty);
        }
        if command.command_id.as_ref().is_some_and(|id| id.len() > 64) {
            return Err(CommandError::IdTooLong);
        }
        Ok(command)
    }
}

#[derive(Debug, Serialize)]
pub struct Ack<'a> {
    pub event: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub command_id: Option<&'a str>,
    pub result: i32,
    pub led: Color,
    pub paused: bool,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Telemetry {
    pub event: &'static str,
    pub firmware: &'static str,
    pub sequence: u64,
    pub random: u32,
    pub uptime_ms: u64,
    pub position_ms: u32,
    pub duration_ms: u32,
    pub paused: bool,
    pub rssi: i32,
    pub heap: u32,
    pub audio_errors: u64,
    pub data_errors: u64,
    pub skipped_frames: u64,
    pub rejected_commands: u64,
    pub dropped_commands: u32,
    pub stack_free: u32,
    pub led: Color,
}

/// Version, pause flag, reserved bytes, LE time/position, then 32 normalized bands.
pub fn spectrum(due: Due, bands: &[u8; BANDS]) -> [u8; 44] {
    let mut bytes = [0; 44];
    bytes[0] = 1;
    bytes[1] = u8::from(due.paused);
    bytes[4..8].copy_from_slice(&due.pts_ms.to_le_bytes());
    bytes[8..12].copy_from_slice(&(due.index * FRAME_MS).to_le_bytes());
    if !due.paused {
        bytes[12..].copy_from_slice(bands);
    }
    bytes
}
