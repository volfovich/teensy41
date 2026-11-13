// AnalogDelay.h - a bucket-brigade (BBD) delay pedal in the manner of the Memory Man and
// the DM-2 (mono in, the same signal on both outputs: the pedals are mono). Written for
// this project after the model in C. Raffel and J. O. Smith, "Practical Modeling of
// Bucket-Brigade Device Circuits", DAFx-10, 2010.
//
//   in -> (+) -> low-pass -> compressor 2:1 -> line -> the chips' soft clip -> expander 1:2
//          ^ x Repeats                                 -> low-pass, third order -+-> out
//          +----------------------------------------------------------------------+
//
//   - The low-passes are the pedal's anti-alias and reconstruction filters: dark repeats,
//     darker each time round. Their corner follows Tone (1.5 to 4.5 kHz) and falls by up
//     to 30 % for times above 300 ms, as the chip's clock is slowed for long delays.
//   - Compressor and expander (the compander chip) follow the signal's level with a 12 ms
//     lag, so they do not cancel on an attack: the repeats swell in a little.
//   - Mod is the chorus on the repeats (0.5 Hz, up to 2.5 ms each way).
//   - A time change glides: the clock changes speed and the repeats bend in pitch.
//
// Threading model: process() and clearStep() run in the audio interrupt. The setters only
// write single values; the filters are recomputed inside process() when Tone or the time
// change (update(), from flash).
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <dsp/Blocks.h>
#include <dsp/fx/FxParts.h>

namespace dsp {

class AnalogDelay {
 public:
  static constexpr int kMinDelay = 128;  // samples

  explicit AnalogDelay(float sampleRate)
      : fs_(sampleRate), glideK_(1.0f / (0.15f * sampleRate)), envK_(onePoleK(1.0f / (6.2831853f * 0.012f), sampleRate)) {
    lfo_.setRate(0.5f, sampleRate);
    setFeedback(0.35f);
  }

  // Hands over the line (len floats) and clears it. The longest delay is len - 4 samples.
  void init(float* line, int len);
  bool ready() const { return a_ != nullptr; }
  int maxDelay() const { return len_ - 4; }
  void reset() {
    clearPos_ = 0;
    clearStep(len_);
  }
  // Clears up to n samples of the line per call; returns true when it is silent.
  bool clearStep(int n);

  // Delay time in samples, clamped to kMinDelay .. maxDelay(); a change glides (with a silent line it
  // applies at once).
  void setDelaySamples(float d) {
    const float hi = (float)maxDelay();
    dSet_ = d < (float)kMinDelay ? (float)kMinDelay : (d > hi ? hi : d);
  }
  // Repeats, 0..1; more feedback also lowers the input a little (as StudioDelay).
  void setFeedback(float n) {
    n = clamp01(n);
    feedback_ = n * kFeedbackMax;
    inGainSet_ = 1.0f - 0.6f * n * n * n;
  }
  // Brightness of the repeats, 0..1.
  void setTone(float n) { tone_ = clamp01(n); }
  // Chorus on the repeats, 0..1.
  void setModDepth(float n) { modDepth_ = clamp01(n) * kModS * fs_; }

  // Processes n samples of mono input into the wet-only outputs (both the same).
  void process(const float* in, float* outL, float* outR, int n) {
    if (tone_ != cTone_ || dSet_ != cTime_) update();
    const float fb = feedback_, inSet = inGainSet_, depth = modDepth_, target = dSet_, gk = glideK_;
    const float ek = envK_, poleK = poleK_;
    if (silent_) {  // nothing in the line to bend: start at the set time
      cur_ = target;
      silent_ = false;
    }
    float inGain = inGain_, cur = cur_, envC = envC_, envE = envE_, pole = pole_, y = last_;
    for (int i = 0; i < n; i++) {
      inGain += (inSet - inGain) * 0.25f;
      cur = glide(cur, target, gk);
      lfo_.tick();
      // Out of the line: the chips' soft clip, the expander, the reconstruction filter.
      float r = readHermite(a_, len_, idx_, cur - depth * lfo_.value(0));
      r = softClip(r * kChip) * (1.0f / kChip);
      envE += (fabsf(r) * kLevel - envE) * ek;
      pole += (outLp_.process(r * envE) - pole) * poleK;
      // Into the line: the input and the last output, filtered and compressed.
      const float w = inLp_.process(in[i] * inGain + fb * y);
      float c = w / (envC > kFloor ? envC : kFloor);
      c = c < -kLineMax ? -kLineMax : (c > kLineMax ? kLineMax : c);
      envC += (fabsf(c) * kLevel - envC) * ek;
      a_[idx_] = c;
      if (++idx_ >= len_) idx_ = 0;
      y = pole;
      outL[i] = outR[i] = y * kOutGain;
    }
    inGain_ = inGain;
    cur_ = cur;
    envC_ = envC;
    envE_ = envE;
    pole_ = pole;
    last_ = y;
  }

 private:
  static constexpr float kFeedbackMax = 0.96f;
  static constexpr float kOutGain = 1.2f;  // as StudioDelay, for similar loudness
  static constexpr float kModS = 0.0025f;  // seconds each way at Mod 100 %
  // The compander's unity point: a steady sine of amplitude 0.25 passes the line at its own
  // level (its rectified average x kLevel = 1); louder is squeezed, quieter raised.
  static constexpr float kLevel = 1.0f / (0.63662f * 0.25f);
  static constexpr float kFloor = 0.0625f;  // the compressor's gain stops at +24 dB
  static constexpr float kLineMax = 4.0f;   // headroom of the stored samples
  static constexpr float kChip = 0.66f;     // drive into the soft clip: about 1/16 x^3

  void update();

  const float fs_, glideK_, envK_;
  float* a_ = nullptr;
  int len_ = 0, idx_ = 0, clearPos_ = 0;
  Biquad inLp_, outLp_;
  Lfo lfo_;
  float poleK_ = 0.5f, pole_ = 0.0f, last_ = 0.0f;
  float envC_ = 0.0f, envE_ = 0.0f;
  float feedback_ = 0.0f, inGainSet_ = 1.0f, inGain_ = 1.0f, modDepth_ = 0.0f;
  volatile float tone_ = 0.6f;
  float cTone_ = -1.0f, cTime_ = -1.0f;
  float dSet_ = 0.0f, cur_ = 0.0f;
  bool silent_ = true;  // the line is clear and process() has not run since
};

}  // namespace dsp
