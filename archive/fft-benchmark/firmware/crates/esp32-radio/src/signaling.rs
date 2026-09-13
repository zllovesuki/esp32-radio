//! Autonomous SFU session orchestration through the authenticated Worker API.
use crate::{
    error::{Error, Result},
    platform::{self, Http},
    radio::{Event, Request},
};
use radio_core::signaling::{self, Channels, Started};
use serde::{Serialize, de::DeserializeOwned};
use std::{
    ffi::CStr,
    sync::mpsc::{Receiver, SyncSender, sync_channel},
    thread,
    time::Duration,
};

#[derive(Clone, Copy)]
enum Endpoint {
    Start,
    Channels,
    Ready,
    Heartbeat,
}
impl Endpoint {
    fn path(self) -> &'static CStr {
        match self {
            Self::Start => c"start",
            Self::Channels => c"channels",
            Self::Ready => c"ready",
            Self::Heartbeat => c"heartbeat",
        }
    }
}

struct Client {
    http: Http,
    response: Vec<u8>,
}
impl Client {
    fn new() -> Result<Self> {
        Ok(Self {
            http: Http::open()?,
            response: vec![0; platform::RESPONSE_LIMIT],
        })
    }
    fn post(&mut self, endpoint: Endpoint, body: &[u8]) -> Result<(i32, usize)> {
        let (status, length) = self.http.post(endpoint.path(), body, &mut self.response)?;
        platform::log(&format!(
            "HTTPS {}: status={status}",
            endpoint.path().to_str().expect("constant ASCII path")
        ));
        Ok((status, length))
    }
    fn retry<T: DeserializeOwned>(
        &mut self,
        endpoint: Endpoint,
        body: &impl Serialize,
    ) -> Result<T> {
        // Serialize once: retries retain exactly the same bootId and offer.
        let body =
            serde_json::to_vec(body).map_err(|_| Error::new("request serialization failed"))?;
        for attempt in 0..3 {
            let (status, length) = self.post(endpoint, &body)?;
            if (200..300).contains(&status) {
                return serde_json::from_slice(&self.response[..length])
                    .map_err(|_| Error::new("invalid Worker response"));
            }
            if !signaling::retryable(status) {
                return Err(Error::native("Worker rejected setup", status));
            }
            if attempt < 2 {
                thread::sleep(Duration::from_secs(1 << attempt));
            }
        }
        Err(Error::new("Worker unavailable after setup retries"))
    }
}

pub(crate) fn run(requests: SyncSender<Request>, events: Receiver<Event>) -> Result<()> {
    let mut client = Client::new()?;
    let offer = match events.recv_timeout(Duration::from_secs(20)) {
        Ok(Event::Offer(offer)) => offer,
        _ => return Err(Error::new("local offer unavailable")),
    };
    let boot_id = format!(
        "{:08x}{:08x}{:08x}{:08x}",
        platform::random(),
        platform::random(),
        platform::random(),
        platform::random()
    );
    let started: Started = client.retry(
        Endpoint::Start,
        &serde_json::json!({
            "bootId": boot_id, "sessionDescription": {"type": "offer", "sdp": offer}
        }),
    )?;
    let (identity, answer) = started.validate()?;
    let (reply, received) = sync_channel(1);
    requests
        .try_send(Request::Answer(answer, reply))
        .map_err(|_| Error::new("radio task unavailable"))?;
    received
        .recv_timeout(Duration::from_secs(15))
        .map_err(|_| Error::new("answer application timed out"))??;
    match events.recv_timeout(Duration::from_secs(30)) {
        Ok(Event::Connected) => {}
        _ => return Err(Error::new("WebRTC connection timed out")),
    }
    let channels: Channels = client.retry(Endpoint::Channels, &identity)?;
    channels.validate()?;
    let (reply, received) = sync_channel(1);
    requests
        .try_send(Request::Start(reply))
        .map_err(|_| Error::new("radio task unavailable"))?;
    received
        .recv_timeout(Duration::from_secs(15))
        .map_err(|_| Error::new("playback start timed out"))??;
    let _: serde_json::Map<String, serde_json::Value> = client.retry(Endpoint::Ready, &identity)?;
    platform::recovery_attempt(true);
    platform::log(
        "ON AIR: autonomous Wi-Fi signaling, audio and data; firmware=rust; USB is optional",
    );
    let identity =
        serde_json::to_vec(&identity).map_err(|_| Error::new("identity serialization failed"))?;
    let mut last_success = platform::now_us();
    loop {
        for _ in 0..50 {
            if events.try_iter().any(|event| matches!(event, Event::Lost)) {
                return Err(Error::new("WebRTC transport lost"));
            }
            thread::sleep(Duration::from_millis(100));
        }
        let (status, length) = client.post(Endpoint::Heartbeat, &identity)?;
        let valid = (200..300).contains(&status)
            && serde_json::from_slice::<serde_json::Map<String, serde_json::Value>>(
                &client.response[..length],
            )
            .is_ok();
        if valid {
            last_success = platform::now_us();
        } else if signaling::needs_recovery(status, platform::now_us().saturating_sub(last_success))
        {
            return Err(Error::native("signaling session needs recovery", status));
        }
    }
}
