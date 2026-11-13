//! Prints what the device side reports, without any UI:
//!
//!   cargo run -p namforge-core --example monitor [seconds] [setting-key value]
//!
//! Connects to the unit, prints its settings and every change (turn a MIDI controller or type
//! into the serial shell... from a second program; the port itself is in use here). With a
//! key and a value it also sets that setting once, a second after connecting.

use std::sync::mpsc;
use std::time::{Duration, Instant};

use namforge_core::{Event, Setting};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let seconds: u64 = args.first().and_then(|s| s.parse().ok()).unwrap_or(10);
    let set: Option<(String, i32)> = match (args.get(1), args.get(2).and_then(|v| v.parse().ok())) {
        (Some(k), Some(v)) => Some((k.clone(), v)),
        _ => None,
    };

    let (tx, rx) = mpsc::channel();
    let device = namforge_core::spawn(move |e| {
        let _ = tx.send(e);
    });

    let mut schema: Vec<Setting> = Vec::new();
    let mut values: Vec<i16> = Vec::new();
    let mut set_at: Option<Instant> = None;
    let end = Instant::now() + Duration::from_secs(seconds);
    while Instant::now() < end {
        if let (Some((key, value)), Some(at)) = (&set, set_at) {
            if at.elapsed() >= Duration::from_secs(1) {
                match schema.iter().find(|s| &s.key == key) {
                    Some(s) => {
                        println!("-> set {} ({}) to {}", s.name, s.key, value);
                        device.set_param(s.id, *value);
                    }
                    None => println!("no setting with the key '{key}'"),
                }
                set_at = None;
            }
        }
        let Ok(event) = rx.recv_timeout(Duration::from_millis(100)) else { continue };
        match event {
            Event::Searching { detail } => println!("searching: {detail}"),
            Event::Connected { info } => {
                println!(
                    "connected on {}: firmware {}, protocol {}, block {} at {} Hz, {} settings",
                    info.port, info.firmware, info.protocol, info.block_size, info.sample_rate, info.settings
                );
                set_at = Some(Instant::now());
            }
            Event::Schema { settings } => {
                println!("settings table: {} rows", settings.len());
                schema = settings;
            }
            Event::State { values: v, files } => {
                if values.is_empty() {
                    println!("model:  {}\nir:     {}\npreset: {}{}", files.model, files.ir, files.preset, if files.edited { " (edited)" } else { "" });
                    for s in schema.iter().take(12) {
                        println!("  {:<12} {:<20} {:>5}   ({}..{})", s.key, s.name, v[s.id as usize], s.min, s.max);
                    }
                    println!("  ... and {} more", schema.len().saturating_sub(12));
                }
                values = v;
            }
            Event::Param { id, value } => {
                let name = schema.get(id as usize).map(|s| s.name.as_str()).unwrap_or("?");
                println!("changed: {name} = {value}");
            }
            Event::Files { files } => println!(
                "files: model {} | ir {} | preset {}{}",
                files.model, files.ir, files.preset, if files.edited { " (edited)" } else { "" }
            ),
            Event::Disconnected { reason } => println!("disconnected: {reason}"),
        }
    }
}
