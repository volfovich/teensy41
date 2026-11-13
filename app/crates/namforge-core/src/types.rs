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
    /// The session ended; the device side starts searching again.
    Disconnected { reason: String },
}
