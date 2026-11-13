// PlateReverb.h - a plate reverb after Jon Dattorro's published design ("Effect Design,
// Part 1: Reverberator and Other Filters", J. Audio Eng. Soc. 45 (9), 1997, figure 1), which
// he credits to David Griesinger: dense and bright from the first moment, with no separate
// early reflections. Written for this project from the paper.
//
//   (L+R)/2 -> pre-delay -> 4 all-pass diffusers -+-> tank half A --+
//                                                 +-> tank half B --+-- each feeds the other
//   one half: (+ the other half's end x decay) -> modulated all-pass -> delay -> damping
//             low-pass (Tone) -> x decay -> all-pass -> delay
//   out L / R: seven taps each along the tank's delays and all-passes -> low cut (100 Hz)
//
// The paper gives the lengths for 29,761 Hz. Here they are multiplied by 1.3, not by the
// 1.61 that 48 kHz would ask for, so the tank and the 150 ms pre-delay fit the memory the
// reverbs share (Reverb80::kMemFloats): the plate is about a fifth smaller than the paper's.
//
//   Size   decay time, about 0.5 to 8 s
//   Tone   damping of the tail, 2.5 to 12 kHz
//   Mod    how far the two modulated all-passes move (slow, 0.5 and 0.31 Hz)
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

class PlateReverb {
 public:
  static constexpr int kPreMax = 7205;  // 150 ms + margin
  static constexpr int kInLen[4] = {185, 139, 493, 360};
  // Per tank half: the modulated all-pass, the first delay, the all-pass, the second delay.
  static constexpr int kTankLen[2][4] = {{874, 5789, 2340, 4836}, {1180, 5482, 3453, 4112}};
  static constexpr int kModPad = 32;  // room for the modulated reads
  static constexpr int kTankFloats = 874 + 5789 + 2340 + 4836 + 1180 + 5482 + 3453 + 4112;
  // Total buffer size for init(), in floats.
  static constexpr int kMemFloats = kPreMax + 185 + 139 + 493 + 360 + kTankFloats + 2 * kModPad;

  explicit PlateReverb(float sampleRate) : fs_(sampleRate) {
    lfo_[0].setRate(0.5f, sampleRate);
    lfo_[1].setRate(0.31f, sampleRate);
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
  void setMod(float n) { mod_ = clamp01(n); }
  void setPreDelayMs(float ms) {
    const float s = ms * 0.001f * fs_;
    preDelay_ = s < 0.0f ? 0 : (s > (float)(kPreMax - 2) ? kPreMax - 2 : (int)s);
  }

  // Processes n samples into the wet-only stereo outputs (inL and inR may be the same
  // buffer). At the default settings 100 % wet is about as loud as the input.
  void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    if (size_ != cSize_ || tone_ != cTone_ || mod_ != cMod_) update();
    const float decay = decay_, dd2 = dd2_, damp = dampK_, depth = modDepth_;
    float lpA = lp_[0], lpB = lp_[1];
    for (int i = 0; i < n; i++) {
      pre_[preIdx_] = 0.5f * (inL[i] + inR[i]);
      int r = preIdx_ - preDelay_;
      if (r < 0) r += kPreMax;
      if (++preIdx_ >= kPreMax) preIdx_ = 0;
      float x = pre_[r];
      x = allpass(in_[0], x, 0.75f);
      x = allpass(in_[1], x, 0.75f);
      x = allpass(in_[2], x, 0.625f);
      x = allpass(in_[3], x, 0.625f);

      lfo_[0].tick();
      lfo_[1].tick();
      const float endA = tank_[0][3].oldest(), endB = tank_[1][3].oldest();
      // Half A.
      float v = modAllpass(tank_[0][0], x + decay * endB, (float)kTankLen[0][0] - depth * lfo_[0].value(0));
      float d = tank_[0][1].oldest();
      tank_[0][1].push(v);
      lpA += (d - lpA) * damp;
      tank_[0][3].push(allpass(tank_[0][2], lpA * decay, dd2));
      // Half B.
      v = modAllpass(tank_[1][0], x + decay * endA, (float)kTankLen[1][0] - depth * lfo_[1].value(0));
      d = tank_[1][1].oldest();
      tank_[1][1].push(v);
      lpB += (d - lpB) * damp;
      tank_[1][3].push(allpass(tank_[1][2], lpB * decay, dd2));

      // The paper's output taps (table 2), positions x 1.3.
      const float l = tank_[1][1].tap(346) + tank_[1][1].tap(3866) - tank_[1][2].tap(2487) + tank_[1][3].tap(2595) -
                      tank_[0][1].tap(2587) - tank_[0][2].tap(243) - tank_[0][3].tap(1386);
      const float rr = tank_[0][1].tap(459) + tank_[0][1].tap(4715) - tank_[0][2].tap(1596) + tank_[0][3].tap(3475) -
                       tank_[1][1].tap(2744) - tank_[1][2].tap(436) - tank_[1][3].tap(157);
      outL[i] = hpL_.process(kOut * l);
      outR[i] = hpR_.process(kOut * rr);
    }
    lp_[0] = lpA;
    lp_[1] = lpB;
  }

 private:
  // Output level: 100 % wet about as loud as the input (pink noise, default settings).
  static constexpr float kOut = 0.6f;
  // The measured decay time over the one the loop gain alone would give.
  static constexpr float kDecayTrim = 1.2f;

  // The tank's first all-pass, read at a moving point (linear interpolation). Its gain has
  // the opposite sign to the others (the paper's "decay diffusion 1", 0.70).
  static inline float modAllpass(Ring& r, float in, float delay) {
    const float d = r.tapLinear(delay);
    const float v = in + 0.7f * d;
    r.push(v);
    return d - 0.7f * v;
  }

  void update();

  const float fs_;
  float* mem_ = nullptr;
  float* pre_ = nullptr;
  Ring in_[4];
  Ring tank_[2][4];
  int clearPos_ = 0, preIdx_ = 0, preDelay_ = 1920;
  float lp_[2] = {};
  Lfo lfo_[2];
  Biquad hpL_, hpR_;
  volatile float size_ = 0.5f, tone_ = 0.5f, mod_ = 0.4f;
  float cSize_ = -1.0f, cTone_ = -1.0f, cMod_ = -1.0f;
  float decay_ = 0.5f, dd2_ = 0.5f, dampK_ = 0.5f, modDepth_ = 0.0f;
};

}  // namespace dsp
