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
            if Instant::now() >= at && action == "track" {
                // "track list [folder]", "track find <text>", "track open <path below /TRACKS>",
                // "track cmd <number> [a] [b] [c]" (PROTOCOL.md 4.8), "track send <file> [folder]",
                // "track rename <from> <to>", "track mkdir <path>", "track rmdir <path>",
                // "track delete <path>".
                let text = |i: usize| args.get(i).map(String::as_str).unwrap_or("");
                let number = |i: usize| args.get(i).and_then(|v| v.parse::<f32>().ok()).unwrap_or(0.0);
                match arg.as_str() {
                    "list" => device.track_list(text(3)),
                    "find" => device.track_find(text(3)),
                    "open" => device.track_open(text(3)),
                    "cmd" => device.track_command(number(3) as u8, number(4), number(5), number(6)),
                    "send" => device.send_tracks(vec![text(3).into()], text(4)),
                    "rename" => device.track_rename(text(3), text(4)),
                    "mkdir" => device.track_folder_new(text(3)),
                    "rmdir" => device.track_folder_delete(text(3)),
                    "delete" => device.track_delete(text(3)),
                    other => println!("unknown track action {other}"),
                }
                preset_at = None;
            } else if Instant::now() >= at && action != "preset" {
                match action.as_str() {
                    "send" => device.send_file(arg),
                    "load-model" => device.load_model(arg),
                    "load-ir" => device.load_ir(arg),
                    "delete" => device.delete_file(arg),
                    "tuner" => device.tuner(arg == "on"),
                    "backup" => device.backup(arg),
                    "restore" => device.restore(arg),
                    _ => {}
                }
                if ["send", "load-model", "load-ir", "delete", "tuner", "backup", "restore"].contains(&action.as_str()) {
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
            Event::Job { title, detail, done, total, finished, error } => {
                if finished || done % 10 == 0 {
                    println!("{title}: {done}/{total} {detail}{}{}", if finished { " (finished)" } else { "" }, if error.is_empty() { String::new() } else { format!(" ERROR: {error}") })
                }
            }
            Event::ModelInfo { info } => println!("model info: {info:?}"),
            Event::IrInfo { info } => match info {
                Some(i) => {
                    let (lo, hi) = i.response.iter().fold((f32::MAX, f32::MIN), |(lo, hi), &d| (lo.min(d), hi.max(d)));
                    println!(
                        "IR info: {} Hz, {} ch, {}-bit, {} frames; {} samples in use, gain {:.2}; picture {} columns, response {} points from {lo:.1} to {hi:.1} dB",
                        i.sample_rate, i.channels, i.bits, i.frames, i.length, i.gain, i.wave.len(), i.response.len()
                    )
                }
                None => println!("IR info: none"),
            },
            Event::Tuner { valid, hz, note, octave, cents } => {
                if valid {
                    println!("tuner: {}{octave} {cents:+.1} cents, {hz:.2} Hz", ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"][note as usize % 12])
                } else {
                    println!("tuner: -")
                }
            }
            Event::Models { files } => println!("models: {} on the card", files.len()),
            Event::Irs { files } => println!("IRs: {} on the card", files.len()),
            Event::Card { space } => match space {
                Some(c) => println!("card: {:.1} GB free of {:.1} GB", c.free_kb as f64 / 1048576.0, c.total_kb as f64 / 1048576.0),
                None => println!("card: none"),
            },
            Event::Transfer { name, sent, total, done, error, index, count, .. } => {
                if done || sent == 0 {
                    println!("transfer {index}/{count} {name}: {sent} of {total} bytes{}{}", if done { ", done" } else { "" }, if error.is_empty() { String::new() } else { format!(", {error}") })
                }
            }
            Event::Track { state: None } => println!("track: this firmware has no backing-track player"),
            Event::Track { state: Some(t) } => println!(
                "track: {:?} {} at {:.1} of {:.1} s, tempo {} %, own {:.2} BPM{}, scan {} %, loop {} ({:.2} to {:.2}), {} gaps; {} tracks{}, list {}",
                t.path,
                ["none", "stopped", "playing", "paused"].get(t.playing as usize).unwrap_or(&"?"),
                t.place, t.length, t.tempo, t.own_tempo, if t.sure { "" } else { " (unsure)" }, t.scan, t.loop_state, t.loop_from, t.loop_to, t.gaps,
                t.tracks, if t.reading { ", being read" } else { "" }, t.list_number
            ),
            Event::TrackList { folder, rows } => {
                println!("tracks in /TRACKS/{folder}: {} rows", rows.len());
                for r in rows.iter().take(40) {
                    println!("  {}{}", r.name, if r.folder { format!("/  ({})", r.rows) } else { String::new() });
                }
            }
            Event::TrackFound { text, hits, more } => {
                println!("found for {text:?}: {}{}", hits.len(), if more { "+" } else { "" });
                for h in hits.iter().take(40) {
                    println!("  {h}");
                }
            }
            Event::Refused { message } => println!("refused: {message}"),
            Event::Disconnected { reason } => println!("disconnected: {reason}"),
        }
    }
}
