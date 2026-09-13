//! Validated Worker responses and recovery policy; no HTTP implementation here.
use crate::protocol::{ROBOT_ID, SPECTRUM_ID};
use serde::{Deserialize, Serialize};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Started {
    generation: String,
    session_description: Description,
}

#[derive(Deserialize)]
struct Description {
    r#type: String,
    sdp: String,
}

// SDP contains temporary transport credentials. Never derive Debug on it.
impl std::fmt::Debug for Started {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Started").finish_non_exhaustive()
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Identity {
    generation: String,
}

impl Started {
    pub fn validate(self) -> Result<(Identity, String), &'static str> {
        if self.generation.is_empty()
            || self.generation.len() > 64
            || self.session_description.r#type != "answer"
            || !self.session_description.sdp.starts_with("v=0")
            || self.session_description.sdp.len() > 16_000
            || self.session_description.sdp.contains('\0')
        {
            return Err("invalid publisher answer");
        }
        Ok((
            Identity {
                generation: self.generation,
            },
            self.session_description.sdp,
        ))
    }
}

#[derive(Debug, Deserialize)]
pub struct Channels {
    channels: Vec<Channel>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Channel {
    data_channel_name: String,
    id: u16,
}

impl Channels {
    /// The native SDK allocates streams in creation order and cannot select IDs.
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.channels.len() != 2
            || self
                .channels
                .iter()
                .filter(|c| c.data_channel_name == "robot" && c.id == ROBOT_ID)
                .count()
                != 1
            || self
                .channels
                .iter()
                .filter(|c| c.data_channel_name == "spectrum" && c.id == SPECTRUM_ID)
                .count()
                != 1
        {
            return Err("unexpected data channel allocation");
        }
        Ok(())
    }
}

pub fn retryable(status: i32) -> bool {
    status < 0 || status == 429 || status >= 500
}

/// A generation rejected by the Worker or 55 seconds without success needs a new boot.
pub fn needs_recovery(status: i32, since_success_us: u64) -> bool {
    ((400..500).contains(&status) && status != 429) || since_success_us > 55_000_000
}
