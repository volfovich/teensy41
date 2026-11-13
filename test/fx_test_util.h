// Helpers shared by the host tests of the reverbs and delays written for this project
// (test_plate_reverb, test_spring_reverb, test_tape_delay, test_analog_delay, test_fx_banks).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dsp/Blocks.h>

namespace fxtest {

constexpr float kFs = 48000.0f;
constexpr int kS = 48000;
constexpr int kB = 32;
constexpr float kPi2 = 6.2831853f;

inline std::vector<float> noise(int n, uint32_t seed, float amp) {
    std::vector<float> v(n);
    uint32_t s = seed * 2654435761u + 1u;
    for (int i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        v[i] = amp * ((float)(s >> 8) * (2.0f / 16777216.0f) - 1.0f);
    }
    return v;
}

// Paul Kellet's economy pink filter on white noise (closer to a guitar), as test_reverb80.
inline std::vector<float> pink(int n, uint32_t seed, float amp) {
    const std::vector<float> w = noise(n, seed, 1.0f);
    std::vector<float> v(n);
    float b0 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < n; i++) {
        b0 = 0.99765f * b0 + w[i] * 0.0990460f;
        b1 = 0.96300f * b1 + w[i] * 0.2965164f;
        b2 = 0.57000f * b2 + w[i] * 1.0526913f;
        v[i] = (b0 + b1 + b2 + w[i] * 0.1848f) * amp * 0.25f;
    }
    return v;
}

inline std::vector<float> sine(float hz, float amp, int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = amp * sinf(kPi2 * hz * (float)i / kFs);
    return v;
}

inline std::vector<float> impulse(int n, float amp = 1.0f, int at = 0) {
    std::vector<float> v(n, 0.0f);
    v[at] = amp;
    return v;
}

inline double rms(const std::vector<float>& v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; i++) s += (double)v[i] * v[i];
    return std::sqrt(s / (double)(to - from));
}

inline double rms2(const std::vector<float>& l, const std::vector<float>& r, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; i++) s += 0.5 * ((double)l[i] * l[i] + (double)r[i] * r[i]);
    return std::sqrt(s / (double)(to - from));
}

inline double db(double ratio) { return 20.0 * std::log10(ratio > 1e-12 ? ratio : 1e-12); }

inline float peak(const std::vector<float>& v, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = from; i < to; i++) p = fmaxf(p, fabsf(v[i]));
    return p;
}

inline bool finiteAndBelow(const std::vector<float>& v, float limit) {
    for (float x : v)
        if (!std::isfinite(x) || std::fabs(x) >= limit) return false;
    return true;
}

// v through a band-pass at hz (two passes).
inline std::vector<float> band(const std::vector<float>& v, float hz, float q = 4.0f) {
    dsp::Biquad a, b;
    a.setBandpass(hz, kFs, q);
    b.setBandpass(hz, kFs, q);
    std::vector<float> o(v.size());
    for (size_t i = 0; i < v.size(); i++) o[i] = b.process(a.process(v[i]));
    return o;
}

// The level of the band at hz, in v[from, to).
inline double bandRms(const std::vector<float>& v, float hz, size_t from, size_t to) {
    return rms(band(v, hz), from, to);
}

// Decay time (to -60 dB) of an impulse response by Schroeder's backward integration, from
// the slope between -5 and -25 dB. Returns seconds.
inline double rt60(const std::vector<float>& l, const std::vector<float>& r, size_t from) {
    std::vector<double> e(l.size() + 1, 0.0);
    for (size_t i = l.size(); i-- > from;) e[i] = e[i + 1] + (double)l[i] * l[i] + (double)r[i] * r[i];
    const double e0 = e[from];
    size_t t5 = from, t25 = from;
    for (size_t i = from; i < l.size(); i++) {
        if (e[i] > e0 * std::pow(10.0, -0.5)) t5 = i;
        if (e[i] > e0 * std::pow(10.0, -2.5)) t25 = i;
    }
    return 3.0 * (double)(t25 - t5) / kFs;
}

// Correlation of the two sides over [from, to).
inline double correlation(const std::vector<float>& l, const std::vector<float>& r, size_t from, size_t to) {
    double ll = 0, rr = 0, lr = 0;
    for (size_t i = from; i < to; i++) {
        ll += (double)l[i] * l[i];
        rr += (double)r[i] * r[i];
        lr += (double)l[i] * r[i];
    }
    return lr / std::sqrt(ll * rr + 1e-30);
}

// The time (samples from `from`) around which a band's energy sits in v[from, to).
inline double centreTime(const std::vector<float>& v, float hz, size_t from, size_t to) {
    const std::vector<float> b = band(v, hz);
    double e = 0, te = 0;
    for (size_t i = from; i < to; i++) {
        const double p = (double)b[i] * b[i];
        e += p;
        te += p * (double)(i - from);
    }
    return te / (e + 1e-30);
}

// The largest step between neighbouring samples.
inline float largestStep(const std::vector<float>& v, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = from + 1; i < to; i++) p = fmaxf(p, fabsf(v[i] - v[i - 1]));
    return p;
}

}  // namespace fxtest
