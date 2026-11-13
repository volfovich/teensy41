//! NAM Forge's device side: finds the unit on USB, keeps a session open over the link protocol
//! (`protocol/PROTOCOL.md`) and reports everything the UI needs as [`Event`]s.
//!
//! - [`session`] speaks the protocol over any byte stream (the serial port, or a fake device
//!   in tests).
//! - [`device`] runs a session on a background thread, reconnects when the unit is unplugged,
//!   and takes commands from the UI.

pub mod device;
pub mod session;
pub mod types;

pub use device::{spawn, DeviceHandle};
pub use types::{DeviceInfo, Event, Files, Meters, Setting};
