#pragma once
// Frames of the desktop app's link protocol (protocol/PROTOCOL.md, section 2).
//
//   F1 D5 | type | flags | sequence (u16) | length (u32) | payload | CRC-32 (u32)
//
// All fields little-endian. The CRC is the common CRC-32 (zlib, IEEE 802.3) over everything
// from the type byte to the end of the payload.
//
// The app shares the USB serial port with the typed command shell. A frame starts with two
// bytes that typed text never contains, so FrameParser can split one byte stream into frames
// and text.
//
// Portable C++, no Arduino: the firmware feeds it bytes from Serial, the host tests feed it
// arrays.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace applink {

constexpr uint8_t kMagic0 = 0xF1;
constexpr uint8_t kMagic1 = 0xD5;
constexpr uint32_t kMaxPayload = 65536;   // the protocol's limit; a parser's buffer may be smaller
constexpr size_t kHeaderSize = 10;        // magic .. length
constexpr size_t kCrcSize = 4;
constexpr size_t kOverhead = kHeaderSize + kCrcSize;

// Flags.
constexpr uint8_t kFlagReply = 0x01;   // answers the request with the same sequence number
constexpr uint8_t kFlagError = 0x02;   // with kFlagReply: the request failed (payload: code + message)
constexpr uint8_t kFlagEvent = 0x04;   // sent unprompted by the device; sequence is 0

struct FrameHeader {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t sequence = 0;
    uint32_t length = 0;
};

// CRC-32 (zlib). Pass the previous result as `crc` to continue over more data.
inline uint32_t crc32(const void* data, size_t n, uint32_t crc = 0) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Writes a whole frame into out. Returns its size, or 0 if it does not fit in outCap or the
// payload is longer than the protocol allows.
inline size_t encodeFrame(uint8_t type, uint8_t flags, uint16_t sequence, const void* payload, uint32_t length,
                          uint8_t* out, size_t outCap) {
    if (length > kMaxPayload || outCap < kOverhead + length) return 0;
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = type;
    out[3] = flags;
    out[4] = static_cast<uint8_t>(sequence);
    out[5] = static_cast<uint8_t>(sequence >> 8);
    out[6] = static_cast<uint8_t>(length);
    out[7] = static_cast<uint8_t>(length >> 8);
    out[8] = static_cast<uint8_t>(length >> 16);
    out[9] = static_cast<uint8_t>(length >> 24);
    if (length) std::memcpy(out + kHeaderSize, payload, length);
    const uint32_t crc = crc32(out + 2, kHeaderSize - 2 + length);
    uint8_t* c = out + kHeaderSize + length;
    c[0] = static_cast<uint8_t>(crc);
    c[1] = static_cast<uint8_t>(crc >> 8);
    c[2] = static_cast<uint8_t>(crc >> 16);
    c[3] = static_cast<uint8_t>(crc >> 24);
    return kOverhead + length;
}

// Splits a byte stream into frames and text, a byte at a time.
//
// The payload is collected in a buffer the caller provides.
//
// After any bad frame the parser no longer knows where frames start and end, and bytes of a
// damaged frame must never reach the text shell, where they could run as a command. So it
// reports the error once (kBadCrc, or kTooLong for a length over the buffer or kMaxPayload)
// and then swallows everything until idle() is called. The caller calls idle() when no byte
// has arrived for a while; that also abandons a frame that stopped half way. The sender's
// part of the bargain: after a request goes unanswered, pause before sending again.
class FrameParser {
public:
    enum class Result : uint8_t {
        kText,      // the byte is not part of a frame: give it to the text shell
        kConsumed,  // part of a frame (or swallowed after an error); nothing to do yet
        kFrame,     // a frame is complete and its CRC is right: see header() and payload()
        kBadCrc,    // a frame ended with the wrong CRC; swallowing until idle()
        kTooLong,   // a frame's length is more than the buffer holds; swallowing until idle()
    };

    FrameParser(uint8_t* payloadBuffer, uint32_t capacity) : buf_(payloadBuffer), cap_(capacity) {}

    Result feed(uint8_t byte) {
        switch (state_) {
        case State::kIdle:
            if (byte == kMagic0) {
                state_ = State::kMagic;
                return Result::kConsumed;
            }
            return Result::kText;
        case State::kMagic:
            if (byte == kMagic1) {
                state_ = State::kHeader;
                got_ = 0;
                return Result::kConsumed;
            }
            // Not a frame after all. The lone first magic byte is not text (it is not a
            // character anyone types), so it is dropped; this byte is looked at afresh.
            state_ = State::kIdle;
            return feed(byte);
        case State::kHeader:
            raw_[got_++] = byte;
            if (got_ < kHeaderSize - 2) return Result::kConsumed;
            header_.type = raw_[0];
            header_.flags = raw_[1];
            header_.sequence = static_cast<uint16_t>(raw_[2] | (raw_[3] << 8));
            header_.length = static_cast<uint32_t>(raw_[4]) | (static_cast<uint32_t>(raw_[5]) << 8) |
                             (static_cast<uint32_t>(raw_[6]) << 16) | (static_cast<uint32_t>(raw_[7]) << 24);
            if (header_.length > kMaxPayload || header_.length > cap_) {
                state_ = State::kSwallow;
                return Result::kTooLong;
            }
            crc_ = crc32(raw_, kHeaderSize - 2);
            got_ = 0;
            state_ = header_.length ? State::kPayload : State::kCrc;
            return Result::kConsumed;
        case State::kPayload:
            buf_[got_++] = byte;
            if (got_ == header_.length) {
                crc_ = crc32(buf_, header_.length, crc_);
                got_ = 0;
                state_ = State::kCrc;
            }
            return Result::kConsumed;
        case State::kCrc:
            raw_[got_++] = byte;
            if (got_ < kCrcSize) return Result::kConsumed;
            {
                const uint32_t sent = static_cast<uint32_t>(raw_[0]) | (static_cast<uint32_t>(raw_[1]) << 8) |
                                      (static_cast<uint32_t>(raw_[2]) << 16) | (static_cast<uint32_t>(raw_[3]) << 24);
                if (sent == crc_) {
                    state_ = State::kIdle;
                    return Result::kFrame;
                }
                state_ = State::kSwallow;
                return Result::kBadCrc;
            }
        case State::kSwallow:
            return Result::kConsumed;
        }
        return Result::kText;
    }

    // No byte for a while: drop a half-received frame and stop swallowing.
    void idle() { state_ = State::kIdle; }

    // True from a frame's first byte until it is complete, and while swallowing, until idle().
    bool busy() const { return state_ != State::kIdle; }

    // Valid after feed() returned kFrame, until the next frame starts.
    const FrameHeader& header() const { return header_; }
    const uint8_t* payload() const { return buf_; }

private:
    enum class State : uint8_t { kIdle, kMagic, kHeader, kPayload, kCrc, kSwallow };

    uint8_t* buf_;
    uint32_t cap_;
    State state_ = State::kIdle;
    uint32_t got_ = 0;
    uint32_t crc_ = 0;
    uint8_t raw_[kHeaderSize] = {};
    FrameHeader header_;
};

} // namespace applink
