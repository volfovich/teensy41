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

static METER_FRAMES: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

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

    // "preset new <name>", "preset recall <name>", "preset save", "preset rename <name>",
    // "preset delete <name>": sent one second after the start.
    let preset: Option<(String, String)> =
        (args.get(1).map(String::as_str) == Some("preset")).then(|| (args.get(2).cloned().unwrap_or_default(), args.get(3).cloned().unwrap_or_default()));
    let mut preset_at = Some(Instant::now() + Duration::from_secs(1));

    let mut schema: Vec<Setting> = Vec::new();
    let mut values: Vec<i16> = Vec::new();
    let mut set_at: Option<Instant> = None;
    let end = Instant::now() + Duration::from_secs(seconds);
    while Instant::now() < end {
        // "send <file on the Mac>", "load-model <path>", "load-ir <path>", "delete <path on the card>".
        if let (Some(action), Some(arg), Some(at)) = (args.get(1), args.get(2), preset_at) {
            if Instant::now() >= at && action != "preset" {
                match action.as_str() {
                    "send" => device.send_file(arg),
                    "load-model" => device.load_model(arg),
                    "load-ir" => device.load_ir(arg),
                    "delete" => device.delete_file(arg),
                    "tuner" => device.tuner(arg == "on"),
                    _ => {}
                }
                if ["send", "load-model", "load-ir", "delete", "tuner"].contains(&action.as_str()) {
                    preset_at = None;
                }
            }
        }
        if let (Some((action, name)), Some(at)) = (&preset, preset_at) {
            if Instant::now() >= at {
                preset_at = None;
                match action.as_str() {
                    "new" => device.new_preset(name),
                    "recall" => device.recall_preset(name),
                    "save" => device.save_preset(),
                    "rename" => device.rename_preset(name),
                    "delete" => device.delete_preset(name),
                    other => println!("unknown preset action {other}"),
                }
            }
        }
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
            Event::Meters { meters: m } => {
                // About one line a second out of 30 frames.
                let n = METER_FRAMES.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if n % 30 == 0 {
                    println!(
                        "meters: in {:.1} dBFS, out {:.1} dBFS, gate {:.2}, cpu {:.1}% (max {:.1}%), amp {:.1}%, clipped {}{}",
                        20.0 * m.input_peak.max(1e-6).log10(),
                        20.0 * m.output_peak.max(1e-6).log10(),
                        m.gate_gain, m.cpu, m.cpu_max, m.cpu_amp, m.clipped, if m.tuner_open { ", tuner open" } else { "" }
                    );
                }
            }
            Event::Presets { presets } => {
                println!("presets: {}", presets.iter().map(|p| p.name.as_str()).collect::<Vec<_>>().join(", "))
            }
            Event::PresetsChanged => {}
            Event::Tuner { valid, hz, note, octave, cents } => {
                if valid {
                    println!("tuner: {}{octave} {cents:+.1} cents, {hz:.2} Hz", ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"][note as usize % 12])
                } else {
                    println!("tuner: -")
                }
            }
            Event::Models { files } => println!("models: {} on the card", files.len()),
            Event::Irs { files } => println!("IRs: {} on the card", files.len()),
            Event::Transfer { name, sent, total, done, error } => {
                if done || sent == 0 {
                    println!("transfer {name}: {sent} of {total} bytes{}{}", if done { ", done" } else { "" }, if error.is_empty() { String::new() } else { format!(", {error}") })
                }
            }
            Event::Refused { message } => println!("refused: {message}"),
            Event::Disconnected { reason } => println!("disconnected: {reason}"),
        }
    }
}
