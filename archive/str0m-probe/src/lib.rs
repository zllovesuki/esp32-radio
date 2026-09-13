//! Isolated feasibility probe; not part of the maintained firmware.
#![forbid(unsafe_code)]

pub mod driver;

use std::{sync::Arc, time::Instant};
use str0m::{Rtc, channel::ChannelConfig, channel::Reliability, crypto::dtls::DtlsCert};

pub fn rtc(certificate: DtlsCert) -> Rtc {
    Rtc::builder()
        .set_crypto_provider(Arc::new(str0m_rust_crypto::default_provider()))
        .set_dtls_cert(certificate)
        .build(Instant::now())
}

pub fn channel(label: &str, id: u16, reliable: bool) -> ChannelConfig {
    ChannelConfig {
        label: label.to_owned(),
        negotiated: Some(id),
        ordered: reliable,
        reliability: if reliable {
            Reliability::Reliable
        } else {
            Reliability::MaxRetransmits { retransmits: 0 }
        },
        ..Default::default()
    }
}
