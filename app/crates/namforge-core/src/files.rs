//! Checks on a file before it is sent to the unit (docs/design/APP_PLAN.md section 6). The unit makes the real
//! decision when it loads the file; these catch the common mistakes early, with a plain message.

use std::path::Path;

/// The number of weights of the A2-Lite network (`dsp::nama2::kWeightCount` in the firmware).
const A2_LITE_WEIGHTS: usize = 1871;

/// What a file is and where it goes on the unit's card.
#[derive(Debug, Clone, PartialEq)]
pub struct Checked {
    /// The path on the card: "/AMP/name.nam" or "/IR/name.wav".
    pub device_path: String,
}

/// Decides from the file's name and contents where it goes, or says why it cannot be used.
/// `max_path` is the longest path the unit accepts (`HELLO`).
pub fn check(local: &Path, data: &[u8], max_path: usize) -> Result<Checked, String> {
    let name = local.file_name().and_then(|n| n.to_str()).ok_or("The file's name cannot be used on the unit.")?;
    let ext = local.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    let (folder, is_model) = match ext.as_str() {
        "nam" => ("/AMP/", true),
        "wav" => ("/IR/", false),
        _ => return Err("Only .nam amp models and .wav impulse responses can be sent.".into()),
    };
    if name.starts_with('.') || name.chars().any(|c| c.is_control() || "/\\:".contains(c)) {
        return Err("The file's name cannot be used on the unit.".into());
    }
    let device_path = format!("{folder}{name}");
    if device_path.len() > max_path {
        return Err(format!("The file's name is too long for the unit (at most {} characters).", max_path - folder.len()));
    }
    if is_model {
        check_model(data)?;
    } else {
        check_wav(data)?;
    }
    Ok(Checked { device_path })
}

/// The longest whole path of a backing track ("/TRACKS/Rock/Song.mp3"), the longest path below
/// `/TRACKS` and the longest name of a track or folder, in bytes, as the unit takes them.
pub const TRACK_PATH_MAX: usize = 263;
pub const TRACK_BELOW_MAX: usize = 255;
pub const TRACK_NAME_MAX: usize = 127;

fn name_ok(name: &str) -> bool {
    !name.is_empty() && !name.starts_with('.') && !name.chars().any(|c| c.is_control() || "/\\:".contains(c))
}

/// The whole path on the card of a track or folder given by its path below `/TRACKS`
/// ("Rock/Song.mp3" gives "/TRACKS/Rock/Song.mp3"), or why the unit would not take it.
pub fn track_card_path(below: &str) -> Result<String, String> {
    if below.is_empty() || !below.split('/').all(|part| name_ok(part) && part != "..") {
        return Err("That name cannot be used on the unit (no empty names, none starting with a dot, no / \\ or :).".into());
    }
    if below.split('/').any(|part| part.len() > TRACK_NAME_MAX) {
        return Err(format!("A name is too long for the unit (at most {TRACK_NAME_MAX} bytes)."));
    }
    let path = format!("/TRACKS/{below}");
    if below.len() > TRACK_BELOW_MAX || path.len() > TRACK_PATH_MAX {
        return Err(format!("The path is too long for the unit (at most {TRACK_BELOW_MAX} bytes below /TRACKS)."));
    }
    Ok(path)
}

/// Checks a backing track before it is sent: `folder` is where it goes below `/TRACKS` (""
/// for `/TRACKS` itself), `size` the file's size. The unit decides when it opens the track
/// whether it can play it; only the name, the path and the size are looked at here.
pub fn check_track(local: &Path, folder: &str, size: u64) -> Result<Checked, String> {
    let name = local.file_name().and_then(|n| n.to_str()).ok_or("The file's name cannot be used on the unit.")?;
    let ext = local.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    if ext != "wav" && ext != "mp3" {
        return Err("Only .wav and .mp3 files can be sent as backing tracks.".into());
    }
    if !name_ok(name) {
        return Err("The file's name cannot be used on the unit.".into());
    }
    if name.len() > TRACK_NAME_MAX {
        return Err(format!("The file's name is too long for the unit (at most {TRACK_NAME_MAX} bytes)."));
    }
    if size > u32::MAX as u64 {
        return Err("The file is too large to send (4 GB at most).".into());
    }
    let below = if folder.is_empty() { name.to_string() } else { format!("{folder}/{name}") };
    Ok(Checked { device_path: track_card_path(&below)? })
}

/// A NAM A2 file (a container with a Lite network) or a plain WaveNet file in the Lite shape.
fn check_model(data: &[u8]) -> Result<(), String> {
    let json: serde_json::Value =
        serde_json::from_slice(data).map_err(|_| "This is not a NAM model file (it is not valid JSON).".to_string())?;
    let is_lite = |m: &serde_json::Value| {
        m["architecture"] == "WaveNet" && m["weights"].as_array().is_some_and(|w| w.len() == A2_LITE_WEIGHTS)
    };
    let lite = match json["architecture"].as_str() {
        Some("SlimmableContainer") => {
            json["config"]["submodels"].as_array().is_some_and(|subs| subs.iter().any(|s| is_lite(&s["model"])))
        }
        Some("WaveNet") => is_lite(&json),
        Some(other) => return Err(format!("This model's architecture is {other}; the unit plays NAM A2 models (A2-Lite).")),
        None => return Err("This is not a NAM model file.".into()),
    };
    if !lite {
        return Err("This model has no A2-Lite network. The unit plays NAM A2 models; an A1 or a Full-only model will not work.".into());
    }
    if let Some(rate) = json["sample_rate"].as_f64() {
        if (rate - 48000.0).abs() > 1.0 {
            return Err(format!("This model was trained at {rate:.0} Hz; the unit runs at 48000 Hz."));
        }
    }
    Ok(())
}

fn check_wav(data: &[u8]) -> Result<(), String> {
    if data.len() < 44 || &data[0..4] != b"RIFF" || &data[8..12] != b"WAVE" {
        return Err("This is not a WAV file.".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wavenet(weights: usize) -> String {
        format!(r#"{{"architecture":"WaveNet","config":{{}},"weights":[{}],"sample_rate":48000}}"#, vec!["0.5"; weights].join(","))
    }

    #[test]
    fn models() {
        let p = Path::new("/tmp/My Amp.nam");
        let lite = wavenet(A2_LITE_WEIGHTS);
        assert_eq!(check(p, lite.as_bytes(), 95).unwrap(), Checked { device_path: "/AMP/My Amp.nam".into() });
        let container = format!(
            r#"{{"architecture":"SlimmableContainer","config":{{"submodels":[{{"max_value":0.5,"model":{lite}}},{{"max_value":1.0,"model":{}}}]}},"weights":[]}}"#,
            wavenet(9000)
        );
        assert!(check(p, container.as_bytes(), 95).is_ok());
        assert!(check(p, wavenet(9000).as_bytes(), 95).unwrap_err().contains("no A2-Lite"));
        assert!(check(p, br#"{"architecture":"LSTM","weights":[]}"#, 95).unwrap_err().contains("LSTM"));
        assert!(check(p, b"not json", 95).unwrap_err().contains("not a NAM"));
        assert!(check(p, lite.replace("48000", "44100").as_bytes(), 95).unwrap_err().contains("44100"));
    }

    #[test]
    fn the_firmware_example_model_passes() {
        let data = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../test/fixtures/nam/A2.nam")).unwrap();
        assert!(check(Path::new("A2.nam"), &data, 95).is_ok());
    }

    #[test]
    fn wavs_and_names() {
        let mut wav = b"RIFF\0\0\0\0WAVEfmt ".to_vec();
        wav.resize(64, 0);
        assert_eq!(check(Path::new("cab.WAV"), &wav, 95).unwrap().device_path, "/IR/cab.WAV");
        assert!(check(Path::new("cab.wav"), b"RIFFxxxx", 95).is_err());
        assert!(check(Path::new("notes.txt"), b"hello", 95).unwrap_err().contains("Only .nam"));
        assert!(check(Path::new(".hidden.wav"), &wav, 95).is_err());
        assert!(check(Path::new(&format!("{}.wav", "x".repeat(100))), &wav, 95).unwrap_err().contains("too long"));
    }

    #[test]
    fn tracks() {
        let ok = |p: &str, folder: &str| check_track(Path::new(p), folder, 5_000_000).map(|c| c.device_path);
        assert_eq!(ok("/tmp/Song.MP3", "").unwrap(), "/TRACKS/Song.MP3");
        assert_eq!(ok("/tmp/Intro take 2.wav", "Rock/Live").unwrap(), "/TRACKS/Rock/Live/Intro take 2.wav");
        assert!(ok("amp.nam", "").unwrap_err().contains("Only .wav and .mp3"));
        assert!(ok(".hidden.mp3", "").is_err());
        assert!(ok("a.mp3", "Rock/.cache").is_err());
        assert!(ok("a.mp3", "Rock//Live").is_err());
        assert!(ok("a.mp3", "../AMP").is_err());
        // A name of 127 bytes goes, one of 128 does not; "é" is two bytes.
        assert!(ok(&format!("{}.mp3", "x".repeat(123)), "").is_ok());
        assert!(ok(&format!("{}.mp3", "x".repeat(124)), "").unwrap_err().contains("127"));
        assert!(ok(&format!("{}é.mp3", "x".repeat(122)), "").is_err());
        // The path below /TRACKS: 255 bytes at most (the whole path is then 263).
        let folder = format!("{}/{}", "a".repeat(120), "b".repeat(120));
        assert_eq!(ok(&format!("{}.mp3", "c".repeat(9)), &folder).unwrap().len(), TRACK_PATH_MAX);
        assert!(ok(&format!("{}.mp3", "c".repeat(10)), &folder).unwrap_err().contains("255"));
        assert!(check_track(Path::new("big.wav"), "", u32::MAX as u64).is_ok());
        assert!(check_track(Path::new("big.wav"), "", u32::MAX as u64 + 1).unwrap_err().contains("too large"));

        assert_eq!(track_card_path("Rock/Live").unwrap(), "/TRACKS/Rock/Live");
        assert!(track_card_path("").is_err());
        assert!(track_card_path("Rock/").is_err());
    }
}
