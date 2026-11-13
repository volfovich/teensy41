//! The NAM Forge link protocol: frames, payload fields and message numbers.
//!
//! The reference is `protocol/PROTOCOL.md`. The firmware's side is `lib/applink`
//! (`Frame.h`, `Wire.h`, `Messages.h`); keep the two in step. Both are checked against the
//! same examples in `protocol/test_frames.txt`.
//!
//! This crate does no I/O. The app core reads bytes from the serial port and feeds them to
//! [`FrameParser`]; it writes what [`encode_frame`] returns.

pub const MAGIC: [u8; 2] = [0xF1, 0xD5];
/// The protocol's payload limit.
pub const MAX_PAYLOAD: usize = 65536;
/// The largest payload the app sends in one frame (the device's buffer holds at least this).
pub const MAX_PAYLOAD_TO_DEVICE: usize = 16384;
pub const HEADER_SIZE: usize = 10;
pub const CRC_SIZE: usize = 4;
pub const PROTOCOL_VERSION: u16 = 1;

/// After a request goes unanswered, wait this long before sending again: the device discards
/// everything after a bad frame until its line has been idle (PROTOCOL.md, section 2.3).
pub const RESYNC_PAUSE_MS: u64 = 250;

pub mod flags {
    pub const REPLY: u8 = 0x01;
    pub const ERROR: u8 = 0x02;
    pub const EVENT: u8 = 0x04;
}

/// Message numbers (PROTOCOL.md, section 4).
pub mod msg {
    pub const HELLO: u8 = 0x01;
    pub const GET_SCHEMA: u8 = 0x02;
    pub const GET_STATE: u8 = 0x03;
    pub const SET_PARAM: u8 = 0x10;
    pub const PARAM_CHANGED: u8 = 0x11;
    pub const STATE_CHANGED: u8 = 0x12;
    pub const PRESET_LIST: u8 = 0x20;
    pub const PRESET_RECALL: u8 = 0x21;
    pub const PRESET_SAVE: u8 = 0x22;
    pub const PRESET_NEW: u8 = 0x23;
    pub const PRESET_RENAME: u8 = 0x24;
    pub const PRESET_DELETE: u8 = 0x25;
    pub const PRESETS_CHANGED: u8 = 0x26;
    pub const MODEL_LIST: u8 = 0x30;
    pub const MODEL_LOAD: u8 = 0x31;
    pub const MODEL_INFO: u8 = 0x32;
    pub const IR_LIST: u8 = 0x38;
    pub const IR_LOAD: u8 = 0x39;
    pub const IR_INFO: u8 = 0x3A;
    pub const FILE_PUT_BEGIN: u8 = 0x40;
    pub const FILE_PUT_DATA: u8 = 0x41;
    pub const FILE_PUT_END: u8 = 0x42;
    pub const FILE_GET: u8 = 0x43;
    pub const FILE_DATA: u8 = 0x44;
    pub const FILE_DELETE: u8 = 0x45;
    pub const FILE_RENAME: u8 = 0x46;
    pub const DIR_LIST: u8 = 0x47;
    pub const CARD_INFO: u8 = 0x48;
    pub const RESCAN: u8 = 0x49;
    pub const SUBSCRIBE: u8 = 0x50;
    pub const METERS: u8 = 0x51;
    pub const TUNER_OPEN: u8 = 0x52;
    pub const TUNER: u8 = 0x53;
    pub const LOG: u8 = 0x60;
}

/// The code in an error reply (PROTOCOL.md, section 5).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorCode {
    UnknownMessage,
    BadPayload,
    NotFound,
    NoCard,
    BadPath,
    Busy,
    Io,
    Checksum,
    Full,
    Version,
    /// A code this version of the app does not know.
    Other(u16),
}

impl ErrorCode {
    pub fn from_u16(code: u16) -> Self {
        match code {
            1 => Self::UnknownMessage,
            2 => Self::BadPayload,
            3 => Self::NotFound,
            4 => Self::NoCard,
            5 => Self::BadPath,
            6 => Self::Busy,
            7 => Self::Io,
            8 => Self::Checksum,
            9 => Self::Full,
            10 => Self::Version,
            other => Self::Other(other),
        }
    }
}

/// CRC-32 (zlib, IEEE 802.3). Pass the previous result as `crc` to continue over more data.
pub fn crc32(data: &[u8], crc: u32) -> u32 {
    let mut crc = !crc;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

/// A decoded frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub msg_type: u8,
    pub flags: u8,
    pub sequence: u16,
    pub payload: Vec<u8>,
}

impl Frame {
    pub fn is_reply(&self) -> bool {
        self.flags & flags::REPLY != 0
    }
    pub fn is_error(&self) -> bool {
        self.flags & flags::ERROR != 0
    }
    pub fn is_event(&self) -> bool {
        self.flags & flags::EVENT != 0
    }
}

/// Builds a whole frame. `None` if the payload is longer than the protocol allows.
pub fn encode_frame(msg_type: u8, flags: u8, sequence: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if payload.len() > MAX_PAYLOAD {
        return None;
    }
    let mut out = Vec::with_capacity(HEADER_SIZE + payload.len() + CRC_SIZE);
    out.extend_from_slice(&MAGIC);
    out.push(msg_type);
    out.push(flags);
    out.extend_from_slice(&sequence.to_le_bytes());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(payload);
    let crc = crc32(&out[2..], 0);
    out.extend_from_slice(&crc.to_le_bytes());
    Some(out)
}

/// What one byte fed to [`FrameParser`] turned out to be.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Fed {
    /// Not part of a frame: text from the device's command shell.
    Text(u8),
    /// Part of a frame (or discarded after an error); nothing to do yet.
    Consumed,
    /// A frame is complete and its CRC is right.
    Frame(Frame),
    /// A frame ended with the wrong CRC. Everything is discarded until [`FrameParser::idle`].
    BadCrc,
    /// A frame's length is over the limit. Everything is discarded until [`FrameParser::idle`].
    TooLong,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    Magic,
    Header,
    Payload,
    Crc,
    Swallow,
}

/// Splits a byte stream into frames and text, a byte at a time. Same rules as the firmware's
/// `applink::FrameParser` (PROTOCOL.md, sections 2.2 and 2.3).
#[derive(Debug)]
pub struct FrameParser {
    state: State,
    max_payload: usize,
    raw: Vec<u8>,
    header: (u8, u8, u16, usize),
    payload: Vec<u8>,
    crc: u32,
}

impl Default for FrameParser {
    fn default() -> Self {
        Self::new(MAX_PAYLOAD)
    }
}

impl FrameParser {
    /// `max_payload` is the longest payload accepted (at most [`MAX_PAYLOAD`]).
    pub fn new(max_payload: usize) -> Self {
        Self {
            state: State::Idle,
            max_payload: max_payload.min(MAX_PAYLOAD),
            raw: Vec::with_capacity(HEADER_SIZE),
            header: (0, 0, 0, 0),
            payload: Vec::new(),
            crc: 0,
        }
    }

    pub fn feed(&mut self, byte: u8) -> Fed {
        match self.state {
            State::Idle => {
                if byte == MAGIC[0] {
                    self.state = State::Magic;
                    Fed::Consumed
                } else {
                    Fed::Text(byte)
                }
            }
            State::Magic => {
                if byte == MAGIC[1] {
                    self.state = State::Header;
                    self.raw.clear();
                    Fed::Consumed
                } else {
                    // Not a frame after all: the lone first magic byte is dropped and this
                    // byte is looked at afresh.
                    self.state = State::Idle;
                    self.feed(byte)
                }
            }
            State::Header => {
                self.raw.push(byte);
                if self.raw.len() < HEADER_SIZE - 2 {
                    return Fed::Consumed;
                }
                let length = u32::from_le_bytes([self.raw[4], self.raw[5], self.raw[6], self.raw[7]]) as usize;
                self.header = (self.raw[0], self.raw[1], u16::from_le_bytes([self.raw[2], self.raw[3]]), length);
                if length > self.max_payload {
                    self.state = State::Swallow;
                    return Fed::TooLong;
                }
                self.crc = crc32(&self.raw, 0);
                self.payload.clear();
                self.raw.clear();
                self.state = if length > 0 { State::Payload } else { State::Crc };
                Fed::Consumed
            }
            State::Payload => {
                self.payload.push(byte);
                if self.payload.len() == self.header.3 {
                    self.crc = crc32(&self.payload, self.crc);
                    self.raw.clear();
                    self.state = State::Crc;
                }
                Fed::Consumed
            }
            State::Crc => {
                self.raw.push(byte);
                if self.raw.len() < CRC_SIZE {
                    return Fed::Consumed;
                }
                let sent = u32::from_le_bytes([self.raw[0], self.raw[1], self.raw[2], self.raw[3]]);
                if sent == self.crc {
                    self.state = State::Idle;
                    Fed::Frame(Frame {
                        msg_type: self.header.0,
                        flags: self.header.1,
                        sequence: self.header.2,
                        payload: std::mem::take(&mut self.payload),
                    })
                } else {
                    self.state = State::Swallow;
                    Fed::BadCrc
                }
            }
            State::Swallow => Fed::Consumed,
        }
    }

    /// No byte for a while: drop a half-received frame and stop discarding.
    pub fn idle(&mut self) {
        self.state = State::Idle;
    }

    /// True from a frame's first byte until it is complete, and while discarding.
    pub fn busy(&self) -> bool {
        self.state != State::Idle
    }
}

/// Writes payload fields (PROTOCOL.md, section 3).
#[derive(Debug, Default, Clone)]
pub struct Writer {
    buf: Vec<u8>,
}

impl Writer {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn u8(&mut self, v: u8) -> &mut Self {
        self.buf.push(v);
        self
    }
    pub fn bool(&mut self, v: bool) -> &mut Self {
        self.u8(v as u8)
    }
    pub fn u16(&mut self, v: u16) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn i16(&mut self, v: i16) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn i32(&mut self, v: i32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn f32(&mut self, v: f32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn bytes(&mut self, v: &[u8]) -> &mut Self {
        self.buf.extend_from_slice(v);
        self
    }
    /// A string of at most 65535 bytes with no zero byte inside; anything else is an error.
    pub fn str(&mut self, v: &str) -> Result<&mut Self, WireError> {
        if v.len() > u16::MAX as usize || v.as_bytes().contains(&0) {
            return Err(WireError::BadString);
        }
        self.u16(v.len() as u16);
        self.buf.extend_from_slice(v.as_bytes());
        Ok(self)
    }
    pub fn finish(self) -> Vec<u8> {
        self.buf
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WireError {
    /// The payload ended before the field did.
    TooShort,
    /// A string that is too long, contains a zero byte, or is not UTF-8.
    BadString,
    /// Bytes were left over after the last field.
    LeftOver,
}

impl std::fmt::Display for WireError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            WireError::TooShort => "the payload is too short",
            WireError::BadString => "a string in the payload is not valid",
            WireError::LeftOver => "the payload has bytes left over",
        })
    }
}

impl std::error::Error for WireError {}

/// Reads payload fields (PROTOCOL.md, section 3).
#[derive(Debug, Clone)]
pub struct Reader<'a> {
    data: &'a [u8],
}

impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8], WireError> {
        if n > self.data.len() {
            return Err(WireError::TooShort);
        }
        let (head, tail) = self.data.split_at(n);
        self.data = tail;
        Ok(head)
    }
    pub fn u8(&mut self) -> Result<u8, WireError> {
        Ok(self.take(1)?[0])
    }
    pub fn bool(&mut self) -> Result<bool, WireError> {
        Ok(self.u8()? != 0)
    }
    pub fn u16(&mut self) -> Result<u16, WireError> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }
    pub fn u32(&mut self) -> Result<u32, WireError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    pub fn i16(&mut self) -> Result<i16, WireError> {
        Ok(self.u16()? as i16)
    }
    pub fn i32(&mut self) -> Result<i32, WireError> {
        Ok(self.u32()? as i32)
    }
    pub fn f32(&mut self) -> Result<f32, WireError> {
        Ok(f32::from_bits(self.u32()?))
    }
    pub fn str(&mut self) -> Result<String, WireError> {
        let n = self.u16()? as usize;
        let bytes = self.take(n)?;
        if bytes.contains(&0) {
            return Err(WireError::BadString);
        }
        String::from_utf8(bytes.to_vec()).map_err(|_| WireError::BadString)
    }
    pub fn bytes(&mut self, n: usize) -> Result<&'a [u8], WireError> {
        self.take(n)
    }
    /// Everything that is left.
    pub fn rest(&mut self) -> &'a [u8] {
        std::mem::take(&mut self.data)
    }
    pub fn remaining(&self) -> usize {
        self.data.len()
    }
    /// Call after the last field: an error if bytes are left over.
    pub fn done(&self) -> Result<(), WireError> {
        if self.data.is_empty() {
            Ok(())
        } else {
            Err(WireError::LeftOver)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Example {
        name: String,
        msg_type: u8,
        flags: u8,
        sequence: u16,
        payload: Vec<u8>,
        frame: Vec<u8>,
    }

    fn from_hex(h: &str) -> Vec<u8> {
        if h == "-" {
            return Vec::new();
        }
        (0..h.len() / 2).map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap()).collect()
    }

    /// The examples shared with the firmware's tests.
    fn examples() -> Vec<Example> {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../protocol/test_frames.txt");
        let text = std::fs::read_to_string(path).expect("protocol/test_frames.txt");
        let list: Vec<Example> = text
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .map(|l| {
                let p: Vec<&str> = l.split(" | ").collect();
                assert_eq!(p.len(), 6, "bad line: {l}");
                Example {
                    name: p[0].to_string(),
                    msg_type: u8::from_str_radix(p[1], 16).unwrap(),
                    flags: u8::from_str_radix(p[2], 16).unwrap(),
                    sequence: p[3].parse().unwrap(),
                    payload: from_hex(p[4]),
                    frame: from_hex(p[5]),
                }
            })
            .collect();
        assert!(list.len() >= 10);
        list
    }

    #[derive(Default)]
    struct Sink {
        parser: FrameParser,
        text: Vec<u8>,
        frames: Vec<Frame>,
        bad_crc: usize,
        too_long: usize,
    }

    impl Sink {
        fn feed(&mut self, bytes: &[u8]) {
            for &b in bytes {
                match self.parser.feed(b) {
                    Fed::Text(t) => self.text.push(t),
                    Fed::Frame(f) => self.frames.push(f),
                    Fed::BadCrc => self.bad_crc += 1,
                    Fed::TooLong => self.too_long += 1,
                    Fed::Consumed => {}
                }
            }
        }
    }

    #[test]
    fn crc32_matches_the_standard_check_value() {
        assert_eq!(crc32(b"123456789", 0), 0xCBF4_3926);
        assert_eq!(crc32(b"", 0), 0);
        assert_eq!(crc32(b"6789", crc32(b"12345", 0)), 0xCBF4_3926);
    }

    #[test]
    fn encodes_every_example_exactly() {
        for e in examples() {
            assert_eq!(encode_frame(e.msg_type, e.flags, e.sequence, &e.payload).unwrap(), e.frame, "{}", e.name);
        }
    }

    #[test]
    fn decodes_every_example() {
        for e in examples() {
            let mut s = Sink::default();
            s.feed(&e.frame);
            assert_eq!(s.frames.len(), 1, "{}", e.name);
            let f = &s.frames[0];
            assert_eq!((f.msg_type, f.flags, f.sequence), (e.msg_type, e.flags, e.sequence), "{}", e.name);
            assert_eq!(f.payload, e.payload, "{}", e.name);
            assert!(s.text.is_empty() && !s.parser.busy(), "{}", e.name);
        }
    }

    #[test]
    fn text_and_frames_mixed() {
        let ex = examples();
        let mut s = Sink::default();
        s.feed(b"s\n");
        for e in &ex {
            s.feed(&e.frame);
            s.feed(b"amp on\r\n");
        }
        assert_eq!(s.frames.len(), ex.len());
        assert_eq!(s.text, [b"s\n".to_vec(), b"amp on\r\n".repeat(ex.len())].concat());
        assert_eq!(s.bad_crc, 0);
    }

    #[test]
    fn a_lone_first_magic_byte_is_dropped() {
        let mut s = Sink::default();
        s.feed(&[b'a', MAGIC[0], b'b', MAGIC[0], MAGIC[0], b'c']);
        assert_eq!(s.text, b"abc");
        s.feed(&[MAGIC[0]]);
        s.feed(&examples()[0].frame);
        assert_eq!(s.frames.len(), 1);
    }

    #[test]
    fn any_flipped_bit_is_rejected_and_nothing_leaks_as_text() {
        let ex = examples();
        let e = &ex[1];
        for byte in 2..e.frame.len() {
            for bit in 0..8 {
                let mut bad = e.frame.clone();
                bad[byte] ^= 1 << bit;
                let mut s = Sink::default();
                s.feed(&bad);
                assert!(s.frames.is_empty() && s.text.is_empty(), "byte {byte} bit {bit}");
                s.parser.idle();
                s.feed(&e.frame);
                s.feed(b"s\n");
                assert_eq!(s.frames.len(), 1);
                assert_eq!(s.text, b"s\n");
            }
        }
    }

    #[test]
    fn bad_crc_discards_until_idle() {
        let ex = examples();
        let mut bad = ex[3].frame.clone();
        *bad.last_mut().unwrap() ^= 0xFF;
        let mut s = Sink::default();
        s.feed(&bad);
        s.feed(&ex[4].frame);
        s.feed(b"drum on\n");
        assert_eq!((s.bad_crc, s.frames.len(), s.text.len()), (1, 0, 0));
        assert!(s.parser.busy());
        s.parser.idle();
        s.feed(&ex[4].frame);
        s.feed(b"ok\n");
        assert_eq!(s.frames.len(), 1);
        assert_eq!(s.text, b"ok\n");
    }

    #[test]
    fn too_long_frame_is_discarded_until_idle() {
        let ex = examples();
        let big = ex.iter().find(|e| e.payload.len() == 300).unwrap();
        let mut s = Sink { parser: FrameParser::new(16), ..Default::default() };
        s.feed(&big.frame);
        s.feed(b"drum on\n");
        assert_eq!((s.too_long, s.frames.len(), s.text.len()), (1, 0, 0));
        s.parser.idle();
        s.feed(b"s");
        s.feed(&ex[0].frame);
        assert_eq!(s.text, b"s");
        assert_eq!(s.frames.len(), 1);
        assert!(encode_frame(1, 0, 0, &vec![0u8; MAX_PAYLOAD + 1]).is_none());
    }

    #[test]
    fn half_a_frame_is_dropped_on_idle() {
        let ex = examples();
        let f = &ex[2].frame;
        let mut s = Sink::default();
        s.feed(&f[..f.len() / 2]);
        assert!(s.parser.busy());
        s.parser.idle();
        s.feed(b"s\n");
        s.feed(f);
        assert_eq!(s.text, b"s\n");
        assert_eq!(s.frames.len(), 1);
    }

    #[test]
    fn wire_round_trip() {
        let mut w = Writer::new();
        w.u8(0xAB).u16(0xBEEF).u32(0xDEAD_BEEF).i16(-2).i32(-380).f32(-12.5).bool(true);
        w.str("Écho").unwrap();
        w.str("").unwrap();
        w.bytes(b"xyz");
        let data = w.finish();
        let mut r = Reader::new(&data);
        assert_eq!(r.u8().unwrap(), 0xAB);
        assert_eq!(r.u16().unwrap(), 0xBEEF);
        assert_eq!(r.u32().unwrap(), 0xDEAD_BEEF);
        assert_eq!(r.i16().unwrap(), -2);
        assert_eq!(r.i32().unwrap(), -380);
        assert_eq!(r.f32().unwrap(), -12.5);
        assert!(r.bool().unwrap());
        assert_eq!(r.str().unwrap(), "Écho");
        assert_eq!(r.str().unwrap(), "");
        assert_eq!(r.bytes(3).unwrap(), b"xyz");
        r.done().unwrap();
    }

    #[test]
    fn wire_reads_the_hello_reply_example() {
        let ex = examples();
        let e = ex.iter().find(|e| e.name == "hello reply").unwrap();
        let mut r = Reader::new(&e.payload);
        assert_eq!(r.u16().unwrap(), PROTOCOL_VERSION);
        assert_eq!(r.str().unwrap(), "0.1.0");
        assert_eq!(r.u16().unwrap(), 32);
        assert_eq!(r.u32().unwrap(), 48000);
        assert_eq!(r.u16().unwrap(), 64);
        assert_eq!(r.u16().unwrap(), 128);
        assert_eq!(r.u16().unwrap(), 95);
        assert_eq!(r.u16().unwrap(), 52);
        assert_eq!(r.u16().unwrap(), 55);
        assert_eq!(r.u32().unwrap(), 0xDEAD_BEEF);
        r.done().unwrap();
    }

    #[test]
    fn wire_errors() {
        let mut r = Reader::new(&[0x05, 0x00, b'a', b'b']);
        assert_eq!(r.str(), Err(WireError::TooShort));
        let mut r = Reader::new(&[0x03, 0x00, b'a', 0x00, b'b']);
        assert_eq!(r.str(), Err(WireError::BadString));
        let mut r = Reader::new(&[0x02, 0x00, 0xFF, 0xFE]);
        assert_eq!(r.str(), Err(WireError::BadString));
        let mut r = Reader::new(&[1, 2, 3]);
        assert_eq!(r.u8().unwrap(), 1);
        assert_eq!(r.done(), Err(WireError::LeftOver));
        assert_eq!(r.u32(), Err(WireError::TooShort));
        assert!(Writer::new().str("a\0b").is_err());
        assert_eq!(ErrorCode::from_u16(8), ErrorCode::Checksum);
        assert_eq!(ErrorCode::from_u16(999), ErrorCode::Other(999));
    }
}
