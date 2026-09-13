use radio_core::{
    music::{Error as MusicError, Pack},
    playback::Playback,
    protocol::{self, Action, Command},
    signaling::{self, Channels, Started},
};
use std::num::NonZeroU32;

// Independent fixture: Python struct.pack('<8sIIIIII32x', ...) and zlib.crc32
// over one 3-byte Opus silence packet with spectrum bytes 0..31. No paid media.
fn fixture() -> Vec<u8> {
    include_bytes!("fixtures/silence.pack").to_vec()
}

#[test]
fn pack_reads_known_crc_and_ignores_erased_partition_tail() {
    let mut bytes = fixture();
    bytes.extend([255; 100]);
    let pack = Pack::parse(&bytes).unwrap();
    let frame = pack.frame(0).unwrap();
    assert_eq!(frame.opus, [0xf8, 0xff, 0xfe]);
    assert_eq!(frame.bands[31], 31);
    assert!(pack.frame(1).is_none());
}

#[test]
fn every_truncated_fixture_is_rejected_without_panicking() {
    let bytes = fixture();
    for end in 0..bytes.len() {
        assert!(
            Pack::parse(&bytes[..end]).is_err(),
            "accepted truncated length {end}"
        );
    }
}

#[test]
fn corrupted_payload_and_hostile_record_lengths_are_rejected() {
    let mut bytes = fixture();
    bytes[100] ^= 1;
    assert_eq!(Pack::parse(&bytes).unwrap_err(), MusicError::Checksum);
    for size in [0u16, 1276, u16::MAX] {
        let mut bytes = fixture();
        bytes[64..66].copy_from_slice(&size.to_le_bytes());
        assert_eq!(Pack::parse(&bytes).unwrap_err(), MusicError::PacketSize);
    }
    for count in [0u32, 30_001, u32::MAX] {
        let mut bytes = fixture();
        bytes[12..16].copy_from_slice(&count.to_le_bytes());
        assert_eq!(Pack::parse(&bytes).unwrap_err(), MusicError::FrameCount);
    }
}

#[test]
fn arbitrary_small_inputs_do_not_panic() {
    let mut random = 123u32;
    for length in 0..1024 {
        let bytes: Vec<u8> = (0..length)
            .map(|_| {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                random as u8
            })
            .collect();
        let _ = Pack::parse(&bytes);
        let _ = Command::parse(&bytes);
    }
}

fn player() -> Playback {
    Playback::new(NonZeroU32::new(5).unwrap(), 1_000_000)
}

#[test]
fn stall_skips_audio_without_bursting_and_wraps_track() {
    let mut player = player();
    assert_eq!(player.due(1_000_000).unwrap().index, 0);
    let due = player.due(1_141_000).unwrap();
    assert_eq!((due.index, due.pts_ms), (2, 140));
    assert_eq!(player.skipped_frames(), 6);
    assert!(player.due(1_141_001).is_none());
    assert_eq!(player.due(1_160_000).unwrap().index, 3);
}

#[test]
fn pause_holds_position_while_rtp_clock_advances() {
    let mut player = player();
    player.due(1_000_000);
    player.apply(Action::Pause);
    let a = player.due(1_020_000).unwrap();
    let b = player.due(1_080_000).unwrap();
    assert_eq!((a.index, b.index, b.pts_ms), (1, 1, 80));
    assert!(a.paused && b.paused);
    player.apply(Action::Play);
    let c = player.due(1_100_000).unwrap();
    assert_eq!((c.index, c.pts_ms, c.paused), (1, 100, false));
}

#[test]
fn restart_resets_song_but_never_resets_transport_clock() {
    let mut player = player();
    player.due(1_000_000);
    player.due(1_020_000);
    player.apply(Action::Pause);
    player.apply(Action::Restart);
    let due = player.due(1_040_000).unwrap();
    assert_eq!((due.index, due.pts_ms, due.paused), (0, 40, false));
}

#[test]
fn spectrum_has_independent_little_endian_wire_oracle() {
    let due = radio_core::playback::Due {
        index: 17,
        pts_ms: 0x12345678,
        paused: false,
        spectrum: true,
    };
    let bytes = protocol::spectrum(due, &[42; 32]);
    assert_eq!(
        &bytes[..12],
        &[1, 0, 0, 0, 0x78, 0x56, 0x34, 0x12, 0x54, 1, 0, 0]
    );
    assert!(bytes[12..].iter().all(|&v| v == 42));
    let paused = protocol::spectrum(
        radio_core::playback::Due {
            paused: true,
            ..due
        },
        &[42; 32],
    );
    assert_eq!(paused[1], 1);
    assert!(paused[12..].iter().all(|&v| v == 0));
}

#[test]
fn commands_enforce_color_and_keep_maintenance_out_of_network_protocol() {
    assert!(Command::parse(br#"{"led":[0,255,17],"command_id":"abc"}"#).is_ok());
    for bytes in [
        br#"{"led":[-1,0,0]}"#.as_slice(),
        br#"{"led":[256,0,0]}"#,
        br#"{"led":[1.5,0,0]}"#,
        br#"{"led":[0,0]}"#,
        br#"{"cmd":"restart_device","led":[0,0,0]}"#,
        br#"{"action":"erase"}"#,
        br#"{"action":"pause","action":"play"}"#,
        br#"{"led":[0,0,0]} trailing"#,
    ] {
        assert!(Command::parse(bytes).is_err());
    }
    assert!(Command::parse(&[b' '; 513]).is_err());
    let oversized_id = format!(r#"{{"action":"pause","command_id":"{}"}}"#, "x".repeat(65));
    assert!(Command::parse(oversized_id.as_bytes()).is_err());
}

#[test]
fn publisher_validation_redacts_sdp_and_rejects_wrong_response_types() {
    let raw = r#"{"generation":"g1","sessionDescription":{"type":"answer","sdp":"v=0\r\na=ice-pwd:secret"}}"#;
    let started: Started = serde_json::from_str(raw).unwrap();
    assert!(!format!("{started:?}").contains("secret"));
    assert!(started.validate().is_ok());
    let wrong: Started = serde_json::from_str(&raw.replace("answer", "offer")).unwrap();
    assert!(wrong.validate().is_err());
}

#[test]
fn channel_allocation_is_checked_by_name_not_response_order() {
    let reversed = r#"{"channels":[{"dataChannelName":"spectrum","id":4},{"dataChannelName":"robot","id":2}]}"#;
    assert!(
        serde_json::from_str::<Channels>(reversed)
            .unwrap()
            .validate()
            .is_ok()
    );
    assert!(
        serde_json::from_str::<Channels>(&reversed.replace("\"id\":2", "\"id\":6"))
            .unwrap()
            .validate()
            .is_err()
    );
    assert!(
        serde_json::from_str::<Channels>(&reversed.replace("spectrum", "robot"))
            .unwrap()
            .validate()
            .is_err()
    );
}

#[test]
fn recovery_distinguishes_transient_failure_and_revoked_generation() {
    assert!(signaling::retryable(-1));
    assert!(signaling::retryable(429));
    assert!(signaling::retryable(503));
    assert!(!signaling::retryable(401));
    assert!(!signaling::needs_recovery(503, 5_000_000));
    assert!(signaling::needs_recovery(503, 56_000_000));
    assert!(signaling::needs_recovery(409, 1));
}
