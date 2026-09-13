//! One owner drives networking, protocol timers, media, and channel writes.

use serde::Serialize;
use std::{collections::HashMap, error::Error, net::UdpSocket, time::Instant};
use str0m::{
    Candidate, Event, Input, Output, Rtc,
    change::{SdpAnswer, SdpPendingOffer},
    channel::ChannelId,
    crypto::dtls::DtlsCert,
    media::{Direction, Frequency, MediaKind, MediaTime},
    net::{Protocol, Receive},
};

pub type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Default, Debug, Serialize)]
pub struct Stats {
    pub audio_sent: u32,
    pub robot_sent: u32,
    pub spectrum_sent: u32,
    pub commands_received: u32,
    pub application_datagrams: u32,
    pub deliberately_dropped: u32,
    pub rtp_datagrams: u32,
}

pub struct Driver {
    rtc: Rtc,
    socket: UdpSocket,
    pending: Option<SdpPendingOffer>,
    mid: str0m::media::Mid,
    channels: HashMap<String, (u16, ChannelId)>,
    pub events: Vec<serde_json::Value>,
    pub stats: Stats,
    pub drop_every: u32,
}

impl Driver {
    pub fn new(ip: &str, certificate: DtlsCert) -> Result<(Self, String)> {
        let socket = UdpSocket::bind((ip, 0))?;
        socket.set_nonblocking(true)?;
        let mut rtc = crate::rtc(certificate);
        rtc.add_local_candidate(Candidate::host(socket.local_addr()?, "udp")?);
        while !matches!(rtc.poll_output()?, Output::Timeout(_)) {}
        let mut change = rtc.sdp_api();
        let mid = change.add_media(MediaKind::Audio, Direction::SendOnly, None, None, None);
        let bootstrap = change.add_channel_with_config(crate::channel("bootstrap", 0, true));
        let (offer, pending) = change.apply().ok_or("no initial offer")?;
        let mut driver = Self {
            rtc,
            socket,
            pending: Some(pending),
            mid,
            channels: HashMap::from([("bootstrap".to_owned(), (0, bootstrap))]),
            events: Vec::new(),
            stats: Stats::default(),
            drop_every: 0,
        };
        driver.drain()?;
        Ok((driver, offer.to_sdp_string()))
    }

    pub fn mid(&self) -> String {
        self.mid.to_string()
    }

    pub fn answer(&mut self, sdp: &str) -> Result<()> {
        let pending = self.pending.take().ok_or("answer already applied")?;
        self.rtc
            .sdp_api()
            .accept_answer(pending, SdpAnswer::from_sdp_string(sdp)?)?;
        self.drain()
    }

    pub fn add_channel(&mut self, label: &str, id: u16, reliable: bool) -> Result<()> {
        let cid = self
            .rtc
            .direct_api()
            .create_data_channel(crate::channel(label, id, reliable));
        self.channels.insert(label.to_owned(), (id, cid));
        self.drain()
    }

    pub fn pump(&mut self) -> Result<()> {
        let mut buffer = [0u8; 2048];
        for _ in 0..32 {
            let (length, source) = match self.socket.recv_from(&mut buffer) {
                Ok(value) => value,
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(error) => return Err(error.into()),
            };
            let receive = Receive {
                proto: Protocol::Udp,
                source,
                destination: self.socket.local_addr()?,
                contents: buffer[..length].try_into()?,
            };
            self.rtc
                .handle_input(Input::Receive(Instant::now(), receive))?;
            self.drain()?;
        }
        self.rtc.handle_input(Input::Timeout(Instant::now()))?;
        self.drain()
    }

    pub fn audio(&mut self, _frame: u32) -> Result<()> {
        let writer = self
            .rtc
            .writer(self.mid)
            .ok_or("audio writer unavailable")?;
        let pt = writer
            .payload_params()
            .next()
            .ok_or("audio codec missing")?
            .pt();
        writer.write(
            pt,
            Instant::now(),
            MediaTime::new(
                u64::from(self.stats.audio_sent) * 960,
                Frequency::FORTY_EIGHT_KHZ,
            ),
            [0xf8, 0xff, 0xfe],
        )?;
        self.stats.audio_sent += 1;
        self.drain()
    }

    pub fn data(&mut self, label: &str, sequence: u32) -> Result<()> {
        let (_, cid) = *self.channels.get(label).ok_or("unknown channel")?;
        let mut channel = self.rtc.channel(cid).ok_or("channel is not open")?;
        let accepted = if label == "robot" {
            let payload =
                serde_json::json!({"kind": "telemetry", "sequence": sequence}).to_string();
            channel.write(false, payload.as_bytes())?
        } else {
            channel.write(true, &sequence.to_le_bytes())?
        };
        if !accepted {
            return Err("channel backpressure".into());
        }
        if label == "robot" {
            self.stats.robot_sent += 1;
        } else {
            self.stats.spectrum_sent += 1;
        }
        self.drain()
    }

    fn drain(&mut self) -> Result<()> {
        loop {
            match self.rtc.poll_output()? {
                Output::Timeout(_) => break,
                Output::Transmit(packet) => {
                    if packet.contents.len() >= 2
                        && packet.contents[0] & 0xc0 == 0x80
                        && !(192..=223).contains(&packet.contents[1])
                    {
                        self.stats.rtp_datagrams += 1;
                    }
                    // DTLS 1.2 application records; enabled only after setup.
                    let application = packet.contents.first() == Some(&23);
                    if application {
                        self.stats.application_datagrams += 1;
                    }
                    if application
                        && self.drop_every != 0
                        && self.stats.application_datagrams % self.drop_every == 0
                    {
                        self.stats.deliberately_dropped += 1;
                    } else {
                        self.socket.send_to(&packet.contents, packet.destination)?;
                    }
                }
                Output::Event(Event::Connected) => {
                    self.events.push(serde_json::json!({"event": "connected"}))
                }
                Output::Event(Event::ChannelOpen(cid, label)) => {
                    let id = self
                        .channels
                        .values()
                        .find(|(_, value)| *value == cid)
                        .map(|(id, _)| *id);
                    self.events.push(
                        serde_json::json!({"event": "channel_open", "label": label, "id": id}),
                    );
                }
                Output::Event(Event::ChannelData(data)) => {
                    let is_robot = self
                        .channels
                        .get("robot")
                        .is_some_and(|(_, cid)| *cid == data.id);
                    if is_robot {
                        self.stats.commands_received += 1;
                        let mut channel = self
                            .rtc
                            .channel(data.id)
                            .ok_or("reply channel unavailable")?;
                        channel.write(data.binary, &data.data)?;
                    }
                }
                Output::Event(_) => {}
            }
        }
        Ok(())
    }
}
