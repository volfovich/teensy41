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

/// The backing-track player's state (PROTOCOL.md 4.8: the reply of `TRACK_OPEN`, `TRACK_COMMAND`
/// and `TRACK_STATE`, and the event `TRACK_CHANGED`). Times are seconds in the track's own time.
#[derive(Debug, Clone, PartialEq, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrackState {
    /// The open track below `/TRACKS` ("Rock/Song.mp3"); "" if none.
    pub path: String,
    /// 0 no track, 1 stopped, 2 playing, 3 paused.
    pub playing: u8,
    pub place: f32,
    pub length: f32,
    /// Percent of the track's own tempo.
    pub tempo: u16,
    /// BPM as the scan found it or as set by hand; 0: not known.
    pub own_tempo: f32,
    /// The scan's confidence is 0.5 or more.
    pub sure: bool,
    /// Percent of the scan for tempo and beats done; 100: done; -1: no track.
    pub scan: i16,
    /// Every beat is at `first_beat` + k × 60 ÷ `own_tempo`.
    pub first_beat: f32,
    pub beats_per_bar: u8,
    /// Loop points go to the nearest beat.
    pub snap: bool,
    /// 0 none, 1 points set and the loop off, 2 looping, 3 the start is marked and waits for its end.
    #[serde(rename = "loop")]
    pub loop_state: u8,
    pub loop_from: f32,
    pub loop_to: f32,
    /// Blocks the unit's queue could not fill, since start.
    pub gaps: u32,
    /// 0 none, 1 WAV, 2 MP3.
    pub file_kind: u8,
    pub file_rate: u32,
    pub channels: u8,
    /// 0 for a WAV.
    pub kbps: u16,
    /// Tracks in the unit's list of the whole tree.
    pub tracks: u16,
    /// The unit is still reading its list from the card.
    pub reading: bool,
    /// Goes up each time the unit starts reading its list anew.
    pub list_number: u32,
}

/// One row of a folder below `/TRACKS` (`TRACK_LIST`).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrackRow {
    pub name: String,
    pub folder: bool,
    /// Rows inside a folder; 0 for a track.
    pub rows: u16,
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
    /// `index` of `count` says which file of a batch it is (from 1; 1 of 1 for a single file), and
    /// `all_sent` of `all_total` are the batch's bytes, this file's included.
    Transfer { name: String, sent: u32, total: u32, done: bool, error: String, index: u32, count: u32, all_sent: u64, all_total: u64 },
    /// The backing-track player's state. `None` once after connecting: this firmware has no
    /// player, and the UI hides everything about tracks.
    Track { state: Option<TrackState> },
    /// The rows of the folder below `/TRACKS` that the window shows ("" is `/TRACKS` itself).
    TrackList { folder: String, rows: Vec<TrackRow> },
    /// The tracks found for `text`, as paths below `/TRACKS`; `more`: the unit found more than
    /// the 256 it lists.
    TrackFound { text: String, hits: Vec<String>, more: bool },
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
