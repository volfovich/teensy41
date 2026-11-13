// Modulation.h - the Mod block: a chorus or a flanger (stereo in, stereo out).
//
//   in L -> line L, read at a delay the LFO sweeps        = wet L
//   in R -> line R, the same LFO a part of a cycle later  = wet R
//   out  = dry x cos(mix) + wet x sin(mix)      (the dry signal is not delayed)
//
//   chorus    the delay moves around 12 ms, by up to +-5 ms at full depth. Above 1 Hz the
//             sweep shrinks with the rate, so the pitch wobble stays the same instead of
//             growing.
//   flanger   the delay moves between 0.3 ms and up to 7 ms at full depth (shrinking
//             above 2 Hz), which sweeps a comb filter's notches through the sound.
//   feedback  some of the wet signal goes back into the line (up to 90 %, low-passed at
//             6 kHz): the flanger's resonance; on the chorus a thicker, ringing sound.
//   width     how far the right side's LFO runs behind the left's: 0 = the same on both
//             sides (mono in stays mono), 1 = half a cycle.
//   mix       0 = the input only, 0.5 = equal parts (the flanger's deepest notches),
//             1 = the wet signal only (vibrato on the chorus). Equal power: at 0.5 both
//             are at 0.707, so where they add up the peak is up to 3 dB higher.
//   level     kept as loud as the input, automatically. Dry and wet at equal power
//             already do that without feedback: the comb's peaks and notches even out
//             over the sweep. Feedback makes the wet signal louder, by 1 / (1 - fb^2)
//             in power (a comb resonator's average gain), so both are turned down by
//             1 / sqrt(dry^2 + wet^2 / (1 - fb^2)). The loop is a little weaker than
//             the feedback setting (its low-pass and the interpolation take some), by a
//             factor measured with band-limited noise: kChorusLoop, kFlangerLoop. This
//             matches loudness, not peaks: a single sine sitting on a comb peak still
//             reads higher.
//
//   - On and off ramp (20 ms) between the input and the effect. Off and settled,
//     process() returns false, writes nothing and costs no CPU. The next on starts from
//     cleared lines and first fills them for 20 ms (longer than any delay here) before
//     the ramp begins: otherwise the wet signal would start with a step, the moment the
//     read reaches the first sample written.
//   - A change of type fades out, restarts the same way with the new type and fades in. The other values glide to their
//     new settings (20 ms), so turning a knob neither clicks nor zips.
//
// Threading model: set() and setOn() run in the background, process() in the audio
// interrupt. set() fills the spare set of values, then switches to it, so a block never
// sees half of one.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <dsp/Blocks.h>
#include <dsp/Port.h>

namespace dsp {

class Modulation {
 public:
  static constexpr int kLen = 2048;  // each side's line, floats (42.7 ms at 48 kHz)
  enum Type { kChorus, kFlanger };
  static constexpr float kMinRateHz = 0.05f, kMaxRateHz = 10.0f;
  static constexpr float kChorusCentreMs = 12.0f, kChorusSweepMs = 5.0f, kChorusKneeHz = 1.0f;
  static constexpr float kFlangerMinMs = 0.3f, kFlangerSweepMs = 6.7f, kFlangerKneeHz = 2.0f;
  static constexpr float kMaxFeedback = 0.9f;
  // The loop's gain relative to the feedback, for the level compensation (measured with
  // noise between 80 Hz and 2.5 kHz, test_modulation).
  static constexpr float kChorusLoop = 0.965f, kFlangerLoop = 0.94f;

  explicit Modulation(float sampleRate)
      : fs_(sampleRate),
        step_(1.0f / (sampleRate * 0.02f)),
        phaseStep_((uint32_t)(2147483648.0 / (sampleRate * 0.05))),
        fbLp_(1.0f - expf(-6.2831853f * 6000.0f / sampleRate)),
        warmLen_((int)(sampleRate * 0.02f)),
        table_(lfoSineTable()) {
    set(kChorus, 0.8f, 0.5f, 0.0f, 0.5f, 0.5f);
  }

  // Hands over the two lines (kLen floats each). Until then the effect stays off.
  void init(float* lineL, float* lineR) {
    memset(lineL, 0, sizeof(float) * kLen);
    memset(lineR, 0, sizeof(float) * kLen);
    lineL_ = lineL;
    lineR_ = lineR;
  }
  bool ready() const { return lineL_ != nullptr; }

  void setOn(bool on) { on_ = on; }
  // Rate in Hz (0.05 - 10); depth, feedback, mix and width 0..1.
  DSP_COLD void set(int type, float rateHz, float depth, float feedback, float mix, float width) {
    Set& s = sets_[1 - cur_];
    rateHz = rateHz < kMinRateHz ? kMinRateHz : (rateHz > kMaxRateHz ? kMaxRateHz : rateHz);
    depth = clamp01(depth);
    s.type = type == kFlanger ? kFlanger : kChorus;
    s.adder = (uint32_t)((double)rateHz * 4294967296.0 / (double)fs_);
    const float ms = fs_ / 1000.0f;
    if (s.type == kFlanger) {
      const float half = 0.5f * depth * kFlangerSweepMs * (rateHz > kFlangerKneeHz ? kFlangerKneeHz / rateHz : 1.0f);
      s.base = (kFlangerMinMs + half) * ms;
      s.sweep = half * ms;
    } else {
      s.base = kChorusCentreMs * ms;
      s.sweep = depth * kChorusSweepMs * (rateHz > kChorusKneeHz ? kChorusKneeHz / rateHz : 1.0f) * ms;
    }
    s.fb = clamp01(feedback) * kMaxFeedback;
    const float m = clamp01(mix) * 1.5707963f;
    s.dry = m <= 0.0f ? 1.0f : cosf(m);
    s.wet = m <= 0.0f ? 0.0f : sinf(m);
    if (s.dry < 1e-6f) s.dry = 0.0f;
    // The level compensation (see the top of the file); 1 without feedback or at mix 0.
    const float loop = s.fb * (s.type == kFlanger ? kFlangerLoop : kChorusLoop);
    s.makeup = 1.0f / sqrtf(s.dry * s.dry + s.wet * s.wet / (1.0f - loop * loop));
    s.dry *= s.makeup;
    s.wet *= s.makeup;
    s.phase = (uint32_t)((double)clamp01(width) * 2147483648.0);  // up to half a cycle
    cur_ = 1 - cur_;
  }

  // One block of n samples; inL and inR may be the same buffer (a mono input). Returns
  // false while idle: outL / outR are not written and the signal is the input.
  bool process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    if (!lineL_) return false;
    const Set& s = sets_[cur_];
    // Starting from idle, or faded out for a change of type: from cleared lines, which
    // fill for warmLen_ samples before the fade-in. The feedback stays at 0 meanwhile and
    // glides up afterwards, so the first sample's step is not sent round the loop.
    if (!running_ || (mix_ == 0.0f && warm_ == 0 && s.type != type_)) {
      if (!running_ && !on_) return false;
      memset(lineL_, 0, sizeof(float) * kLen);
      memset(lineR_, 0, sizeof(float) * kLen);
      running_ = true;
      warm_ = warmLen_;
      mix_ = 0.0f;
      type_ = s.type;
      base_ = s.base;
      sweep_ = s.sweep;
      fb_ = 0.0f;
      dry_ = s.dry;
      wet_ = s.wet;
      phase_ = s.phase;
      lpL_ = lpR_ = 0.0f;
    }
    // A new type waits for the fade-out; then the branch above restarts with it.
    const float mixT = on_ && s.type == type_ ? 1.0f : 0.0f;
    const bool same = s.type == type_;
    const float baseT = same ? s.base : base_, sweepT = same ? s.sweep : sweep_;
    float m = mix_, base = base_, sweep = sweep_, fb = fb_, dry = dry_, wet = wet_, lpL = lpL_, lpR = lpR_;
    uint32_t acc = acc_, phase = phase_;
    int idx = idx_, warm = warm_;
    const float k = step_;
    for (int i = 0; i < n; i++) {
      const float xl = inL[i], xr = inR[i];
      acc += s.adder;
      base += (baseT - base) * k;
      sweep += (sweepT - sweep) * k;
      fb += ((warm > 0 ? 0.0f : s.fb) - fb) * k;
      dry += (s.dry - dry) * k;
      wet += (s.wet - wet) * k;
      if (phase < s.phase) phase += s.phase - phase < phaseStep_ ? s.phase - phase : phaseStep_;
      else if (phase > s.phase) phase -= phase - s.phase < phaseStep_ ? phase - s.phase : phaseStep_;

      const float wl = readHermite(lineL_, kLen, idx, base + sweep * lfo(acc));
      const float wr = readHermite(lineR_, kLen, idx, base + sweep * lfo(acc - phase));
      lpL += (wl - lpL) * fbLp_;
      lpR += (wr - lpR) * fbLp_;
      lineL_[idx] = xl + fb * lpL;
      lineR_[idx] = xr + fb * lpR;
      idx = (idx + 1) & (kLen - 1);

      if (warm > 0) warm--;  // the lines are filling: still the input
      else m = mixT > m ? fminf(m + step_, mixT) : fmaxf(m - step_, mixT);
      // Between the input (m = 0) and the mixed effect (m = 1).
      const float dg = 1.0f + m * (dry - 1.0f), wg = m * wet;
      outL[i] = dg * xl + wg * wl;
      outR[i] = dg * xr + wg * wr;
    }
    acc_ = acc;
    phase_ = phase;
    idx_ = idx;
    warm_ = warm;
    mix_ = m;
    if (!on_ && m == 0.0f) running_ = false;  // faded out: idle from the next block on
    base_ = base;
    sweep_ = sweep;
    fb_ = fb;
    dry_ = dry;
    wet_ = wet;
    lpL_ = lpL;
    lpR_ = lpR;
    return true;
  }

  // For tests: the delay's range being glided to, in samples, the type playing and how
  // far the effect is faded in (1 = fully).
  float delayMin() const { return sets_[cur_].base - sets_[cur_].sweep; }
  float delayMax() const { return sets_[cur_].base + sets_[cur_].sweep; }
  int type() const { return type_; }
  // The level compensation of the settings given last (1 = none).
  float makeup() const { return sets_[cur_].makeup; }
  float fade() const { return mix_; }

 private:
  struct Set {
    int type = kChorus;
    uint32_t adder = 0;  // the LFO's phase step per sample
    uint32_t phase = 0;  // the right side's lag (2^32 = a cycle)
    float base = 0.0f, sweep = 0.0f;  // samples: the delay is base +- sweep
    float fb = 0.0f, dry = 1.0f, wet = 0.0f;  // dry and wet include the makeup
    float makeup = 1.0f;
  };

  // The sine at a 32-bit phase, -1 .. 1.
  inline float lfo(uint32_t acc) const {
    const uint32_t i = acc >> 24;
    const float f = (float)(acc & 0x00FFFFFF) * (1.0f / 16777216.0f);
    return table_[i] + (table_[i + 1] - table_[i]) * f - 1.0f;
  }

  const float fs_, step_;
  const uint32_t phaseStep_;  // the right side's lag moves half a cycle in 50 ms at most
  const float fbLp_;
  const int warmLen_;  // samples the lines fill before the effect fades in
  const float* const table_;
  Set sets_[2];
  volatile int cur_ = 0;
  volatile bool on_ = false;
  float* lineL_ = nullptr;
  float* lineR_ = nullptr;
  // State (audio interrupt only).
  bool running_ = false;
  int idx_ = 0, type_ = kChorus, warm_ = 0;
  uint32_t acc_ = 0, phase_ = 0;
  float mix_ = 0.0f, base_ = 0.0f, sweep_ = 0.0f, fb_ = 0.0f, dry_ = 1.0f, wet_ = 0.0f;
  float lpL_ = 0.0f, lpR_ = 0.0f;
};

}  // namespace dsp
