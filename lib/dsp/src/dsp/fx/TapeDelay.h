// TapeDelay.h - a tape echo (mono in, stereo out), in the manner of the Echoplex and the
// Space Echo. Written for this project; the parts are the usual ones of the published
// models (e.g. S. Arnardottir, J. S. Abel and J. O. Smith, "A Digital Model of the Echoplex
// Tape Delay", AES 125th Convention, 2008).
//
//   in -> (+) -> saturation (Drive) -> line -> playback: low cut (110 Hz), low-pass (Tone) -+-> out
//          ^ x Repeats                                                                       |
//          +---------------------------------------------------------------------------------+
//
//   - The tape is written through a soft clip, so the repeats compress instead of piling up.
//   - Every repeat passes the playback filters again and loses lows and highs.
//   - Wow (0.6 Hz) and flutter (7.3 Hz) move the playback point (Mod). The right side reads
//     the same tape with the wow a quarter turn and the flutter half a turn apart, which
//     gives the repeats a little width; the feedback comes from the left head.
//   - A time change glides (a second or so for a large change), so the repeats bend in pitch as on
//     a machine whose head is moved, instead of StudioDelay's crossfade.
//
// Threading model: process() and clearStep() run in the audio interrupt. The setters only
// write single values; the low-pass is recomputed inside process() when Tone changes
// (update(), from flash).
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <dsp/Blocks.h>
#include <dsp/fx/FxParts.h>

namespace dsp {

class TapeDelay {
 public:
  static constexpr int kMinDelay = 128;  // samples

  explicit TapeDelay(float sampleRate) : fs_(sampleRate), glideK_(1.0f / (0.15f * sampleRate)) {
    wow_.setRate(0.6f, sampleRate);
    flutter_.setRate(7.3f, sampleRate);
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
  // Brightness of the repeats, 0..1 (1.2 to 7 kHz).
  void setTone(float n) { tone_ = clamp01(n); }
  // Wow and flutter, 0..1.
  void setModDepth(float n) {
    n = clamp01(n);
    wowDepth_ = n * kWowS * fs_;
    flutterDepth_ = n * kFlutterS * fs_;
  }
  // How hard the tape is driven, 0..1.
  void setDrive(float n) {
    n = clamp01(n);
    drivePre_ = 1.0f + 9.0f * n * n;
    driveInv_ = 1.0f / drivePre_;
  }

  // Processes n samples of mono input into the wet-only stereo outputs.
  void process(const float* in, float* outL, float* outR, int n) {
    if (tone_ != cTone_) update();
    const float fb = feedback_, inSet = inGainSet_, pre = drivePre_, preInv = driveInv_;
    const float wowD = wowDepth_, fluD = flutterDepth_, hpK = hpK_, target = dSet_, gk = glideK_;
    if (silent_) {  // nothing in the line to bend: start at the set time
      cur_ = target;
      silent_ = false;
    }
    float inGain = inGain_, cur = cur_, hpL = hpL_, hpR = hpR_;
    for (int i = 0; i < n; i++) {
      inGain += (inSet - inGain) * 0.25f;
      cur = glide(cur, target, gk);
      wow_.tick();
      flutter_.tick();
      const float ra = readHermite(a_, len_, idx_, cur - wowD * wow_.value(0) - fluD * flutter_.value(0));
      const float rb = readHermite(a_, len_, idx_, cur - wowD * wow_.value(64) - fluD * flutter_.value(128));
      hpL += (ra - hpL) * hpK;
      hpR += (rb - hpR) * hpK;
      const float yl = lpL_.process(ra - hpL);
      const float yr = lpR_.process(rb - hpR);
      a_[idx_] = softClip((in[i] * inGain + fb * yl) * pre) * preInv;
      if (++idx_ >= len_) idx_ = 0;
      outL[i] = yl * kOutGain;
      outR[i] = yr * kOutGain;
    }
    inGain_ = inGain;
    cur_ = cur;
    hpL_ = hpL;
    hpR_ = hpR;
  }

 private:
  static constexpr float kFeedbackMax = 0.96f;
  static constexpr float kOutGain = 1.2f;  // as StudioDelay, for similar loudness
  static constexpr float kWowS = 0.0013f;      // seconds each way at Mod 100 %: 1 % in pitch
  static constexpr float kFlutterS = 0.000033f;  // 0.3 % in pitch

  void update();

  const float fs_, glideK_;
  float* a_ = nullptr;
  int len_ = 0, idx_ = 0, clearPos_ = 0;
  Biquad lpL_, lpR_;
  Lfo wow_, flutter_;
  float hpL_ = 0.0f, hpR_ = 0.0f, hpK_ = 0.014f;
  float feedback_ = 0.0f, inGainSet_ = 1.0f, inGain_ = 1.0f;
  float wowDepth_ = 0.0f, flutterDepth_ = 0.0f, drivePre_ = 1.0f, driveInv_ = 1.0f;
  volatile float tone_ = 0.6f;
  float cTone_ = -1.0f;
  float dSet_ = 0.0f, cur_ = 0.0f;
  bool silent_ = true;  // the line is clear and process() has not run since
};

}  // namespace dsp
