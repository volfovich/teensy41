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
}
