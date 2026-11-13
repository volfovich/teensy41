#pragma once
// Conversion between raw I2S words and float samples. Portable (no Teensy includes).
//
// Wire format: interleaved L,R 32-bit words with a 24-bit sample in bits 31..8.
// Float format: [-1, 1), where 1.0 corresponds to 2^23 at 24 bits.
// Scaling by 2^23 both ways makes int -> float -> int exact for every 24-bit value.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dsp {

constexpr float kInt24Scale = 8388608.0f;          // 2^23
constexpr float kInt24ToFloat = 1.0f / 8388608.0f;

// One raw word -> float. The low byte is discarded: converters do not guarantee it is zero.
inline float int24WordToFloat(int32_t word) {
    return static_cast<float>(word >> 8) * kInt24ToFloat;
}

// One float -> raw word. Saturates outside [-1, 1), rounds to nearest, maps NaN to 0.
// The low byte of the result is always zero.
inline int32_t floatToInt24Word(float x) {
    float s = x * kInt24Scale;
    if (s != s) s = 0.0f;                           // NaN
    if (s > 8388607.0f) s = 8388607.0f;
    if (s < -8388608.0f) s = -8388608.0f;
    const int32_t v = static_cast<int32_t>(s + std::copysign(0.5f, s));
    return static_cast<int32_t>(static_cast<uint32_t>(v) << 8);
}

// Interleaved raw words -> separate left/right float buffers. n = frames.
inline void int24InToFloat(const int32_t* in, float* left, float* right, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        left[i] = int24WordToFloat(in[2 * i]);
        right[i] = int24WordToFloat(in[2 * i + 1]);
    }
}

// Separate left/right float buffers -> interleaved raw words. n = frames.
// left and right may point to the same buffer (mono to both channels).
inline void floatToInt24Out(const float* left, const float* right, int32_t* out, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = floatToInt24Word(left[i]);
        out[2 * i + 1] = floatToInt24Word(right[i]);
    }
}

} // namespace dsp
