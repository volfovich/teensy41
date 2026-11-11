#pragma once
// Portable float biquad (Direct Form II Transposed) with RBJ "Audio EQ Cookbook" designs.
// No Teensy/Arduino dependencies: this compiles on the host for unit tests.

#include <cmath>
#include <cstddef>

namespace dsp {

constexpr float kPi = 3.14159265358979323846f;

struct BiquadCoeffs {
    // Normalized so a0 == 1.
    // y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    static BiquadCoeffs identity() { return {}; }

    static BiquadCoeffs lowpass(float fs, float f0, float q) {
        const Prep p(fs, f0, q);
        const float b1 = 1.0f - p.cosw;
        return normalize(b1 * 0.5f, b1, b1 * 0.5f, 1.0f + p.alpha, -2.0f * p.cosw, 1.0f - p.alpha);
    }

    static BiquadCoeffs highpass(float fs, float f0, float q) {
        const Prep p(fs, f0, q);
        const float b1 = -(1.0f + p.cosw);
        return normalize(-b1 * 0.5f, b1, -b1 * 0.5f, 1.0f + p.alpha, -2.0f * p.cosw, 1.0f - p.alpha);
    }

    // Constant 0 dB peak gain band-pass.
    static BiquadCoeffs bandpass(float fs, float f0, float q) {
        const Prep p(fs, f0, q);
        return normalize(p.alpha, 0.0f, -p.alpha, 1.0f + p.alpha, -2.0f * p.cosw, 1.0f - p.alpha);
    }

    static BiquadCoeffs notch(float fs, float f0, float q) {
        const Prep p(fs, f0, q);
        return normalize(1.0f, -2.0f * p.cosw, 1.0f, 1.0f + p.alpha, -2.0f * p.cosw, 1.0f - p.alpha);
    }

    static BiquadCoeffs peaking(float fs, float f0, float q, float gainDb) {
        const Prep p(fs, f0, q);
        const float A = std::pow(10.0f, gainDb / 40.0f);
        return normalize(1.0f + p.alpha * A, -2.0f * p.cosw, 1.0f - p.alpha * A,
                         1.0f + p.alpha / A, -2.0f * p.cosw, 1.0f - p.alpha / A);
    }

    // Magnitude response |H(e^jw)| at frequency f (Hz). Handy for tests and debugging.
    float magnitudeAt(float fs, float f) const {
        const float w = 2.0f * kPi * f / fs;
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
        const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const float dr = 1.0f + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

private:
    struct Prep {
        float cosw, alpha;
        Prep(float fs, float f0, float q) {
            const float w0 = 2.0f * kPi * f0 / fs;
            cosw = std::cos(w0);
            alpha = std::sin(w0) / (2.0f * q);
        }
    };

    static BiquadCoeffs normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
        const float inv = 1.0f / a0;
        BiquadCoeffs c;
        c.b0 = b0 * inv; c.b1 = b1 * inv; c.b2 = b2 * inv;
        c.a1 = a1 * inv; c.a2 = a2 * inv;
        return c;
    }
};

class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    const BiquadCoeffs& coeffs() const { return c_; }
    void reset() { z1_ = z2_ = 0.0f; }

    inline float process(float x) {
        const float y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }

    // In-place safe (in == out allowed).
    void process(const float* in, float* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) out[i] = process(in[i]);
    }

private:
    BiquadCoeffs c_;
    float z1_ = 0.0f, z2_ = 0.0f;
};

} // namespace dsp
