// SpringReverb.h - the spring tank of a guitar amplifier: two springs whose echoes are
// smeared into rising chirps (low notes arrive first, the highs trail in up to the spring's
// top frequency), the "drip" of the sound. Written for this project after the structure in
// V. Valimaki, J. Parker and J. S. Abel, "Parametric Spring Reverberation Effect", J. Audio
// Eng. Soc. 58 (7/8), 2010 (its low-frequency chirp loop only, with far fewer all-pass
// stages than the paper's 100, to fit the CPU).
//
//   (L+R)/2 -> low cut (150 Hz) -+-> spring 1 --+-> out L (mostly spring 1)
//                                +-> spring 2 --+-> out R (mostly spring 2)
//   one spring: (+ the other spring's far end x gain, damped) -> 26 stretched all-passes
//               (a + z^-5) / (1 + a z^-5) -> DC blocker -> delay (33 / 42 ms, wobbling)
//               -> its far end -> low-pass, fourth order (Tone): the spring's top, below
//               4.8 kHz -> out
//
// The sound is taken at the springs' far ends, as in a tank, so it starts 33 ms after the
// input (there is no pre-delay setting in this mode).
//
// A stretched all-pass delays the band just under fs / (2 x 5) = 4.8 kHz the most, and
// repeats that pattern above it; the low-pass keeps the first band only. Each spring
// returns into the other, so the sound runs round both (75 ms) before it meets itself: for
// the same decay time that loop needs less gain than two separate springs would, and single
// notes ring out less unevenly.
//
//   Size   decay time, about 0.8 to 3.2 s
//   Tone   the low-pass, 2 to 3.8 kHz
//
// Threading model: process() and clearStep() run in the audio interrupt. The setters only
// write single values; process() recomputes the coefficients when they change (update(),
// from flash).
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <dsp/Blocks.h>
#include <dsp/fx/FxParts.h>

namespace dsp {

class SpringReverb {
 public:
  static constexpr int kSprings = 2;
  static constexpr int kStages = 26;  // all-passes per spring
  static constexpr int kStretch = 5;  // each all-pass's delay, samples
  static constexpr int kLoopLen[kSprings] = {1601, 1999};
  static constexpr int kPad = 16;  // room for the wobble
  // Total buffer size for init(), in floats.
  static constexpr int kMemFloats = 1601 + 1999 + kSprings * kPad;

  explicit SpringReverb(float sampleRate) : fs_(sampleRate) {
    lfo_[0].setRate(0.41f, sampleRate);
    lfo_[1].setRate(0.57f, sampleRate);
  }

  // Hands over the line memory (kMemFloats floats) and clears it.
  void init(float* mem);
  bool ready() const { return mem_ != nullptr; }
  void reset() {
    clearPos_ = 0;
    clearStep(kMemFloats);
  }
  // Clears up to n floats per call; returns true when the reverb is silent.
  bool clearStep(int n);

  void setSize(float n) { size_ = clamp01(n); }
  void setTone(float n) { tone_ = clamp01(n); }

  // Processes n samples into the wet-only stereo outputs (inL and inR may be the same
  // buffer). At the default settings 100 % wet is about as loud as the input.
  void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    if (size_ != cSize_ || tone_ != cTone_) update();
    for (int off = 0; off < n; off += kChunk) {
      const int m = n - off < kChunk ? n - off : kChunk;
      float x[kChunk], y[kSprings][kChunk], end[kSprings][kChunk];
      for (int i = 0; i < m; i++) x[i] = inHp_.process(0.5f * (inL[off + i] + inR[off + i]));
      // The far ends of both springs, read before this chunk is written: each is that
      // spring's output and what returns into the other spring.
      for (int s = 0; s < kSprings; s++) feed(s, x, end[1 - s], y[s], m);
      for (int s = 0; s < kSprings; s++) spring(s, y[s], m);
      for (int i = 0; i < m; i++) {
        const float a = lpB_[0].process(lpA_[0].process(end[0][i]));
        const float b = lpB_[1].process(lpA_[1].process(end[1][i]));
        outL[off + i] = kOut * (a + kCross * b);
        outR[off + i] = kOut * (b + kCross * a);
      }
    }
  }

 private:
  // The delays are far longer than a chunk, so a spring is worked a chunk at a time, one
  // all-pass after the other, with the same result as sample by sample.
  static constexpr int kChunk = 32;
  static constexpr float kOut = 1.0f;     // 100 % wet about as loud as the input (pink noise)
  static constexpr float kCross = 0.15f;  // share of the other spring on each side
  static constexpr float kWobble = 3.0f;  // samples each way
  static constexpr float kStageGain[kSprings] = {0.62f, 0.58f};
  // The measured decay time over the one the loop gain alone would give.
  static constexpr float kDecayTrim = 1.25f;

  // end = the other spring's far end (its delay, wobbling); y = the input plus that, damped
  // and x this spring's gain.
  inline void feed(int s, const float* x, float* end, float* y, int m) {
    const Ring& line = line_[1 - s];
    const int len = kLoopLen[1 - s];
    Lfo& lfo = lfo_[s];
    const float fb = s ? gain_[s] : -gain_[s], damp = dampK_;
    float lp = loopLp_[s];
    for (int i = 0; i < m; i++) {
      lfo.tick();
      const float d = line.tapLinear((float)(len - i) - kWobble * (lfo.value(0) - 1.0f));
      end[i] = d;
      lp += (d - lp) * damp;
      y[i] = x[i] + fb * lp;
    }
    loopLp_[s] = lp;
  }

  // The all-passes and the DC blocker, into the delay.
  inline void spring(int s, const float* y, int m) {
    // An all-pass in its one-multiply form, out[n] = a (in[n] - out[n-5]) + in[n-5]: the
    // chip's multiply-add is what this reverb costs. Each stage's output history is the
    // next one's input history, so two buffers (the last five samples, then the chunk)
    // swap roles from stage to stage.
    const float a = kStageGain[s];
    float bufA[kStretch + kChunk], bufB[kStretch + kChunk];
    float* u = bufA;
    float* w = bufB;
    float (*hist)[kStretch] = hist_[s];
    for (int j = 0; j < kStretch; j++) u[j] = hist[0][j];
    for (int i = 0; i < m; i++) u[kStretch + i] = y[i];
    for (int j = 0; j < kStretch; j++) hist[0][j] = u[m + j];
    for (int k = 1; k <= kStages; k++) {
      for (int j = 0; j < kStretch; j++) w[j] = hist[k][j];
      for (int i = 0; i < m; i++) w[kStretch + i] = a * (u[kStretch + i] - w[i]) + u[i];
      for (int j = 0; j < kStretch; j++) hist[k][j] = w[m + j];
      float* t = u;
      u = w;
      w = t;
    }
    Ring& line = line_[s];
    float dc = dc_[s];
    const float dcK = dcK_;
    for (int i = 0; i < m; i++) {
      const float c = u[kStretch + i];
      dc += (c - dc) * dcK;
      line.push(c - dc);
    }
    dc_[s] = dc;
  }

  void update();

  const float fs_;
  float* mem_ = nullptr;
  Ring line_[kSprings];
  // The last five samples at the chain's input [0] and after each all-pass [1..].
  float hist_[kSprings][kStages + 1][kStretch] = {};
  int clearPos_ = 0;
  float loopLp_[kSprings] = {}, dc_[kSprings] = {};
  float gain_[kSprings] = {};
  float dampK_ = 0.5f, dcK_ = 0.005f;
  Lfo lfo_[kSprings];
  Biquad inHp_, lpA_[kSprings], lpB_[kSprings];
  volatile float size_ = 0.5f, tone_ = 0.5f;
  float cSize_ = -1.0f, cTone_ = -1.0f;
};

}  // namespace dsp
