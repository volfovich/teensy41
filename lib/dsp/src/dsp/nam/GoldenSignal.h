#pragma once
// The test signal behind test/fixtures/nam/A2_lite_ref.f32: 1 s at 48 kHz with two plucked
// notes, a sweep and noise bursts at three levels. Shared by the host test and the on-board
// benchmark so both compare against the same NeuralAmpModelerCore reference output.
// Must stay bit-identical to goldenSignal() in teensy-ir-loader's test/host/test_nam.cpp.

#include <math.h>
#include <stdint.h>

namespace dsp {
namespace nama2 {

constexpr int kGoldenFrames = 48000;

// Fills x[0 .. kGoldenFrames).
inline void goldenSignal(float* x) {
    const int fs = kGoldenFrames;
    for (int i = 0; i < fs; i++) x[i] = 0.0f;
    const float notes[2][2] = {{0.0f, 82.41f}, {0.25f, 110.0f}};
    for (const auto& n : notes) {
        const int s0 = (int)(n[0] * fs);
        for (int i = 0; s0 + i < fs / 2; i++) {
            const float t = (float)i / fs;
            float v = 0.0f;
            for (int h = 1; h < 8; h++) v += sinf(2.0f * 3.14159265f * n[1] * h * t) / h;
            x[s0 + i] += 0.35f * v * expf(-6.0f * t);
        }
    }
    for (int i = 0; i < fs / 4; i++) {   // 50 Hz .. 8 kHz
        const float t = (float)i / (fs / 4);
        x[fs / 2 + i] += 0.3f * sinf(2.0f * 3.14159265f * 50.0f * (powf(160.0f, t) - 1.0f) / logf(160.0f) * 0.25f);
    }
    uint32_t rng = 7;
    const float levels[3] = {0.02f, 0.2f, 0.8f};
    for (int b = 0; b < 3; b++)
        for (int i = 0; i < fs / 12; i++) {
            rng = rng * 1664525u + 1013904223u;
            x[3 * fs / 4 + b * fs / 12 + i] += levels[b] * ((float)(rng >> 8) / 8388608.0f - 1.0f);
        }
}

// The network starts from silence while the reference was pre-warmed, so the two agree only
// after the receptive field has passed. Compare from this frame on.
constexpr int kGoldenCompareFrom = 6400;

} // namespace nama2
} // namespace dsp
