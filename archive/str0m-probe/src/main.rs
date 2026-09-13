use serde::Deserialize;
use std::{
    io::{self, BufRead, Write},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
use str0m::crypto::dtls::DtlsCert;
use str0m_probe::driver::{Driver, Result};

#[derive(Deserialize)]
#[serde(tag = "command", rename_all = "snake_case")]
enum Command {
    Answer { sdp: String },
    Channels { robot: u16, spectrum: u16 },
    Run { frames: u32, drop_every: u32 },
    Warmup { frames: u32 },
    Stats,
    Stop,
}

fn emit(value: &impl serde::Serialize) -> Result<()> {
    let mut out = io::stdout().lock();
    serde_json::to_writer(&mut out, value)?;
    writeln!(out)?;
    out.flush()?;
    Ok(())
}

fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        return Err("expected IP, certificate and key paths".into());
    }
    let cert = DtlsCert {
        certificate: std::fs::read(&args[2])?,
        private_key: std::fs::read(&args[3])?,
    };
    let (mut driver, offer) = Driver::new(&args[1], cert)?;
    // The parent process consumes this private signaling IPC; it is not a log.
    emit(&serde_json::json!({"event": "offer", "sdp": offer, "mid": driver.mid()}))?;
    let (sender, commands) = mpsc::sync_channel(8);
    thread::spawn(move || {
        for line in io::stdin().lock().lines() {
            let Ok(line) = line else {
                break;
            };
            let Ok(command) = serde_json::from_str::<Command>(&line) else {
                break;
            };
            if sender.send(command).is_err() {
                break;
            }
        }
    });
    let started = Instant::now();
    let mut workload: Option<(Instant, u32, u32, bool)> = None;
    while started.elapsed() < Duration::from_secs(60) {
        if let Ok(command) = commands.try_recv() {
            match command {
                Command::Answer { sdp } => driver.answer(&sdp)?,
                Command::Channels { robot, spectrum } => {
                    driver.add_channel("robot", robot, true)?;
                    driver.add_channel("spectrum", spectrum, false)?;
                }
                Command::Run { frames, drop_every } => {
                    driver.drop_every = drop_every;
                    workload = Some((Instant::now(), 0, frames, true));
                }
                Command::Warmup { frames } => workload = Some((Instant::now(), 0, frames, false)),
                Command::Stats => {
                    emit(&serde_json::json!({"event": "stats", "stats": driver.stats}))?
                }
                Command::Stop => {
                    emit(&serde_json::json!({"event": "stopped", "stats": driver.stats}))?;
                    return Ok(());
                }
            }
        }
        driver.pump()?;
        for event in driver.events.drain(..) {
            emit(&event)?;
        }
        if let Some((begin, frame, count, data)) = &mut workload {
            if begin.elapsed() >= Duration::from_millis(u64::from(*frame) * 20) {
                driver.audio(*frame)?;
                if *data && *frame % 5 == 0 {
                    driver.data("robot", *frame / 5)?;
                }
                if *data && *frame % 2 == 0 {
                    driver.data("spectrum", *frame / 2)?;
                }
                *frame += 1;
                if *frame == *count {
                    emit(
                        &serde_json::json!({"event": if *data {"run_done"} else {"warmup_done"}, "stats": driver.stats}),
                    )?;
                    workload = None;
                }
            }
        }
        thread::sleep(Duration::from_millis(1));
    }
    Err("probe deadline exceeded".into())
}

fn main() {
    if run().is_err() {
        // Error payloads can contain SDP. Keep stdout/stderr free of diagnostics.
        let _ = emit(&serde_json::json!({"event": "failed"}));
        std::process::exit(1);
    }
}
