//! What the device tells the app, in the shape the UI receives it (as JSON).

use serde::Serialize;

/// The `HELLO` reply.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceInfo {
    pub protocol: u16,
    pub firmware: String,
    pub block_size: u16,
    pub sample_rate: u32,
    pub max_models: u16,
    pub max_irs: u16,
    pub max_path: u16,
    /// Settings that are stored on the device; the rest are live values (the drums playing).
    pub saved_settings: u16,
    pub settings: u16,
    pub schema_hash: u32,
    /// The serial port the unit was found on.
    pub port: String,
}

/// One row of the device's settings table (`GET_SCHEMA`).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Setting {
    pub id: u16,
    pub key: String,
    pub name: String,
    pub label: String,
    pub min: i16,
    pub max: i16,
    pub default: i16,
    pub step: u8,
    /// PROTOCOL.md section 6.
    pub unit: u8,
    pub in_preset: bool,
    pub choices: Vec<String>,
}

/// The loaded files and the preset's state.
#[derive(Debug, Clone, PartialEq, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Files {
    pub model: String,
    pub ir: String,
    pub preset: String,
    pub edited: bool,
    pub card: bool,
}

/// One `METERS` frame (PROTOCOL.md 4.6). Peaks are linear (1.0 = full scale) and, like
/// `cpu_max`, cover the time since the previous frame.
#[derive(Debug, Clone, PartialEq, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Meters {
    pub input_peak: f32,
    pub output_peak: f32,
    /// The noise gate's gain, 0 (closed) to 1 (open).
    pub gate_gain: f32,
    pub clipped: u32,
    /// Percent of the block period, whole chain.
    pub cpu: f32,
    pub cpu_max: f32,
    pub cpu_amp: f32,
    pub cpu_delay: f32,
    pub cpu_reverb: f32,
    pub cpu_drums: f32,
    pub overruns: u32,
    pub overload_mutes: u32,
    pub amp_running: bool,
    pub delay_ringing: bool,
    pub reverb_ringing: bool,
    pub drums_playing: bool,
    pub muted: bool,
    pub tuner_open: bool,
    /// The peak at each block's output, in the order the unit sends them: amp, gate, cab, delay, reverb, mod, limiter.
    /// Above 1.0 is over full scale. Empty from firmware that does not measure them.
    pub stage_peaks: Vec<f32>,
}

/// A preset on the unit's SD card.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Preset {
    /// The file's name without ".txt": "03 Lead".
    pub name: String,
    /// The number in the name (MIDI program change n recalls preset n + 1); 0 if it has none.
    pub number: u16,
}

/// A model or IR file on the unit's SD card.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileEntry {
    /// "/AMP/JCM 800.nam"
    pub path: String,
    pub size: u32,
}

/// What the loaded amp model's file says about it, and the gains the unit applies (`MODEL_INFO`).
/// A number the file does not have is `None`.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModelInfo {
    pub name: String,
    pub gear_make: String,
    pub gear_model: String,
    pub gear_type: String,
    pub tone_type: String,
    pub sample_rate: f32,
    pub loudness: Option<f32>,
    pub input_level_dbu: Option<f32>,
    pub output_level_dbu: Option<f32>,
    /// dB added at the amp's input to match the capture's level; `None`: calibration is off.
    pub calibration_db: Option<f32>,
    /// dB added at the amp's output to bring every model to the same loudness.
    pub level_db: f32,
}

/// What the loaded IR's file is, and what the unit made of it (`IR_INFO`).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IrInfo {
    pub sample_rate: u32,
    pub channels: u16,
    pub bits: u16,
    pub frames: u32,
    /// Samples in use at 48 kHz, after trimming and cutting.
    pub length: u32,
    pub resampled: bool,
    pub cut: bool,
    pub gain: f32,
    /// The IR in use as a picture: columns of `[lowest, highest]` sample, the largest being 1.
    /// Empty when the firmware cannot send the IR.
    pub wave: Vec<[f32; 2]>,
    /// Its gain in dB at points spread logarithmically from 20 Hz to 20 kHz (see `curve`).
    pub response: Vec<f32>,
}

/// The SD card's size and free space, in KB (`CARD_INFO`).
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CardSpace {
    pub total_kb: u32,
    pub free_kb: u32,
}

/// Everything the device side reports to the UI.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum Event {
    /// Looking for the unit (nothing plugged in, or the port is in use).
    Searching { detail: String },
    Connected { info: DeviceInfo },
    Schema { settings: Vec<Setting> },
    /// Every setting's value, in setting-number order, and the loaded files.
    State { values: Vec<i16>, files: Files },
    Param { id: u16, value: i32 },
    Files { files: Files },
    Meters { meters: Meters },
    Presets { presets: Vec<Preset> },
    /// A tuner reading. `note` is 0 (C) to 11 (B); `cents` is -50 to +50 from that note.
    Tuner { valid: bool, hz: f32, note: u8, octave: i16, cents: f32 },
    Models { files: Vec<FileEntry> },
    /// Details of the loaded model and IR; `None` when none is loaded (or the firmware is older).
    ModelInfo { info: Option<ModelInfo> },
    IrInfo { info: Option<IrInfo> },
    Irs { files: Vec<FileEntry> },
    /// The card's size and free space; `None` without a card (or with older firmware).
    Card { space: Option<CardSpace> },
    /// A file on its way to the unit: `sent` of `total` bytes. `done` with an empty `error` means it
    /// arrived and is in the lists; with an `error`, why it was not sent.
    Transfer { name: String, sent: u32, total: u32, done: bool, error: String },
    /// A backup or a restore under way: `done` of `total` files. `finished` with an empty `error`
    /// means all went well; `detail` is the file being copied, then a summary.
    Job { title: String, detail: String, done: u32, total: u32, finished: bool, error: String },
    /// The list of presets changed on the unit; the device thread fetches it again.
    PresetsChanged,
    /// Something the user asked for was refused ("No such preset").
    Refused { message: String },
    /// The session ended; the device side starts searching again.
    Disconnected { reason: String },
}
