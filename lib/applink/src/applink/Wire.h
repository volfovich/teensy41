#pragma once
// Reading and writing the fields of a frame's payload (protocol/PROTOCOL.md, section 3):
// integers and floats little-endian, a string as a u16 byte count followed by UTF-8.
//
// Both keep an error flag instead of failing at each call: read or write everything, then
// check ok() once. A Reader that ran past the end returns zeros and empty strings.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace applink {

class Writer {
public:
    Writer(uint8_t* buffer, size_t capacity) : buf_(buffer), cap_(capacity) {}

    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        put(b, 2);
    }
    void u32(uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                              static_cast<uint8_t>(v >> 24)};
        put(b, 4);
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    void bytes(const void* data, size_t n) { put(data, n); }
    // nullptr is written as an empty string. A string over 65535 bytes sets the error flag.
    void str(const char* s) {
        const size_t n = s ? std::strlen(s) : 0;
        if (n > 0xFFFF) {
            ok_ = false;
            return;
        }
        u16(static_cast<uint16_t>(n));
        put(s, n);
    }

    bool ok() const { return ok_; }
    size_t size() const { return len_; }
    const uint8_t* data() const { return buf_; }

private:
    void put(const void* p, size_t n) {
        if (!ok_ || n > cap_ - len_) {
            ok_ = false;
            return;
        }
        if (n) std::memcpy(buf_ + len_, p, n);
        len_ += n;
    }

    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t length) : p_(data), left_(length) {}

    uint8_t u8() {
        uint8_t b[1] = {0};
        take(b, 1);
        return b[0];
    }
    uint16_t u16() {
        uint8_t b[2] = {0, 0};
        take(b, 2);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    uint32_t u32() {
        uint8_t b[4] = {0, 0, 0, 0};
        take(b, 4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
               (static_cast<uint32_t>(b[3]) << 24);
    }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    // Copies a string into out (always terminated). One that does not fit in outCap - 1 bytes,
    // or that contains a zero byte, sets the error flag.
    void str(char* out, size_t outCap) {
        const size_t n = u16();
        if (outCap) out[0] = 0;
        if (!ok_ || n > left_ || n + 1 > outCap) {
            ok_ = false;
            return;
        }
        if (std::memchr(p_, 0, n)) {
            ok_ = false;
            return;
        }
        std::memcpy(out, p_, n);
        out[n] = 0;
        p_ += n;
        left_ -= n;
    }
    // The next n bytes in place (no copy), or nullptr with the error flag set.
    const uint8_t* bytes(size_t n) {
        if (!ok_ || n > left_) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t* at = p_;
        p_ += n;
        left_ -= n;
        return at;
    }

    bool ok() const { return ok_; }
    // True if everything was read without error and nothing is left over.
    bool done() const { return ok_ && left_ == 0; }

private:
    void take(uint8_t* out, size_t n) {
        if (!ok_ || n > left_) {
            ok_ = false;
            return;
        }
        std::memcpy(out, p_, n);
        p_ += n;
        left_ -= n;
    }

    const uint8_t* p_;
    size_t left_;
    bool ok_ = true;
};

} // namespace applink
