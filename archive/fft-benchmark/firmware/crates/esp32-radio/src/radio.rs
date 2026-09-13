//! One task owns the peer, playback clock, LED, counters, and packet buffers.
use crate::{
    error::{Error, Result},
    platform::{self, Board, MusicBytes, Peer, PeerState},
};
use radio_core::{
    music::{FRAME_MS, Pack},
    playback::Playback,
    protocol::{self, Ack, Color, Command, ROBOT_ID, SPECTRUM_ID, Telemetry},
};
use std::{
    sync::mpsc::{Receiver, SyncSender},
    thread,
    time::Duration,
};

pub(crate) enum Request {
    Answer(String, SyncSender<Result<()>>),
    Start(SyncSender<Result<()>>),
}
pub(crate) enum Event {
    Offer(String),
    Connected,
    Lost,
}

// SDP-bearing messages intentionally have no Debug implementation.

#[derive(Debug, Default)]
struct Counters {
    audio: u64,
    data: u64,
    rejected: u64,
    sequence: u64,
}

pub(crate) fn run(
    mut board: Board,
    requests: Receiver<Request>,
    events: SyncSender<Event>,
) -> Result<()> {
    let storage = MusicBytes::load()?;
    let pack =
        Pack::parse(storage.as_slice()).map_err(|_| Error::new("music pack validation failed"))?;
    platform::log(&format!(
        "Music validated: frames={} duration_ms={}",
        pack.frame_count(),
        pack.frame_count().get() * FRAME_MS
    ));
    let mut peer = Peer::open()?;
    let mut playback = None;
    let mut led = Color([0, 12, 4]);
    let mut counters = Counters::default();
    let mut next_telemetry = 0;
    let mut previous = PeerState::Connecting;
    let mut offer_buffer = vec![0; 16_000];
    let mut offer_sent = false;
    let mut command_buffer = [0; protocol::COMMAND_LIMIT];
    let mut json_buffer = Vec::with_capacity(768);
    loop {
        if let Ok(request) = requests.try_recv() {
            match request {
                Request::Answer(sdp, reply) => {
                    let _ = reply.send(peer.answer(&sdp));
                }
                Request::Start(reply) => {
                    let result = if playback.is_some() {
                        Err(Error::new("playback already started"))
                    } else {
                        peer.create_channels()
                    };
                    if result.is_ok() {
                        playback = Some(Playback::new(pack.frame_count(), platform::now_us()));
                    }
                    let _ = reply.send(result);
                }
            }
        }
        peer.poll();
        if !offer_sent {
            let length = peer.offer(&mut offer_buffer)?;
            if length > 0 {
                let offer = std::str::from_utf8(&offer_buffer[..length])
                    .map_err(|_| Error::new("invalid local SDP"))?;
                events
                    .try_send(Event::Offer(offer.to_owned()))
                    .map_err(|_| Error::new("signaling event queue unavailable"))?;
                offer_sent = true;
                offer_buffer.fill(0);
                // Release setup-only storage before the steady-state packet loop.
                offer_buffer = Vec::new();
            }
        }
        let state = peer.state();
        if state != previous {
            if state == PeerState::Connected {
                events
                    .try_send(Event::Connected)
                    .map_err(|_| Error::new("signaling event queue unavailable"))?;
                platform::log("WebRTC audio and SCTP connected");
            } else if state == PeerState::Lost {
                let _ = events.try_send(Event::Lost);
                return Err(Error::new("WebRTC transport disconnected"));
            }
            previous = state;
        }
        if let Some(player) = playback.as_mut().filter(|_| state == PeerState::Connected) {
            // Bound per-tick work even if a controller floods the reliable stream.
            let length = peer.command(&mut command_buffer)?;
            if length > 0 {
                match Command::parse(&command_buffer[..length]) {
                    Ok(command) => {
                        let mut result = 0;
                        if let Some(color) = command.led {
                            if board.set_led(color).is_ok() {
                                led = color;
                            } else {
                                result = -1;
                            }
                        }
                        if let Some(action) = command.action {
                            player.apply(action);
                        }
                        let ack = Ack {
                            event: "ack",
                            command_id: command.command_id.as_deref(),
                            result,
                            led,
                            paused: player.paused(),
                        };
                        send_json(&mut peer, &ack, &mut json_buffer, &mut counters)?;
                    }
                    Err(_) => counters.rejected = counters.rejected.saturating_add(1),
                }
            }
            let now = platform::now_us();
            if let Some(due) = player.due(now) {
                let frame = pack
                    .frame(due.index)
                    .ok_or(Error::new("playback index out of range"))?;
                let opus = if due.paused {
                    protocol::SILENCE
                } else {
                    frame.opus
                };
                if peer.audio(due.pts_ms, opus).is_err() {
                    counters.audio = counters.audio.saturating_add(1);
                }
                if due.spectrum
                    && peer
                        .data(SPECTRUM_ID, true, &protocol::spectrum(due, frame.bands))
                        .is_err()
                {
                    counters.data = counters.data.saturating_add(1);
                }
            }
            if now >= next_telemetry {
                next_telemetry = now + 500_000;
                counters.sequence = counters.sequence.wrapping_add(1);
                let telemetry = Telemetry {
                    event: "telemetry",
                    firmware: "rust",
                    sequence: counters.sequence,
                    random: platform::random() % 1000,
                    uptime_ms: now / 1000,
                    position_ms: player.position_ms(),
                    duration_ms: player.duration_ms(),
                    paused: player.paused(),
                    rssi: platform::rssi(),
                    heap: platform::heap_free(),
                    audio_errors: counters.audio,
                    data_errors: counters.data,
                    skipped_frames: player.skipped_frames(),
                    rejected_commands: counters.rejected,
                    dropped_commands: peer.dropped(),
                    stack_free: platform::stack_free(),
                    led,
                };
                send_json(&mut peer, &telemetry, &mut json_buffer, &mut counters)?;
            }
        }
        thread::sleep(Duration::from_millis(1));
    }
}

fn send_json(
    peer: &mut Peer,
    value: &impl serde::Serialize,
    buffer: &mut Vec<u8>,
    counters: &mut Counters,
) -> Result<()> {
    buffer.clear();
    serde_json::to_writer(&mut *buffer, value)
        .map_err(|_| Error::new("packet serialization failed"))?;
    if peer.data(ROBOT_ID, false, buffer).is_err() {
        counters.data = counters.data.saturating_add(1);
    }
    Ok(())
}
