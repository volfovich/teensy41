// FxParts.h - small pieces shared by the reverbs and delays written for this project
// (PlateReverb, SpringReverb, TapeDelay, AnalogDelay) and their banks.
#pragma once

#include <math.h>
#include <stdint.h>

namespace dsp {

// A circular line in memory handed in by the owner. push() writes one sample; tap(t) reads
// the sample pushed t pushes ago (1 = the last one, len = the oldest).
struct Ring {
  float* bf = nullptr;
  int len = 0, idx = 0;
  inline float oldest() const { return bf[idx]; }
  inline void push(float v) {
    bf[idx] = v;
    if (++idx >= len) idx = 0;
  }
  inline float tap(int t) const {
    int i = idx - t;
    if (i < 0) i += len;
    return bf[i];
  }
  // Linear interpolation between tap(floor(d)) and the one before it; 1 <= d <= len - 1.
  inline float tapLinear(float d) const {
    const int i = (int)d;
    const float f = d - (float)i;
    const float a = tap(i), b = tap(i + 1);
    return a + f * (b - a);
  }
};

// Schroeder all-pass on a Ring (delay = its length): flat response, gain k inside.
inline float allpass(Ring& r, float in, float k) {
  const float d = r.oldest();
  const float v = in - k * d;
  r.push(v);
  return d + k * v;
}

// Cubic soft clip: unity gain at 0, flat at +-1 from |x| = 1.5 (as StudioDelay's drive).
inline float softClip(float x) {
  if (x > 1.5f) return 1.0f;
  if (x < -1.5f) return -1.0f;
  return x - (4.0f / 27.0f) * x * x * x;
}

// Coefficient of the one-pole low-pass y += (x - y) * k for a corner frequency.
inline float onePoleK(float hz, float sampleRate) { return 1.0f - expf(-6.2831853f * hz / sampleRate); }

// One sample of a delay time gliding toward its target, as a tape machine or a clocked
// analog line changes speed: a one-pole approach (coefficient k), never faster than half a
// sample per sample, so the pitch of what is in the line bends by an octave at most, and
// never slower than 0.0005 (under a cent), so it arrives.
inline float glide(float cur, float target, float k) {
  const float diff = target - cur;
  const float mag = fabsf(diff);
  float s = mag * k;
  s = s > 0.5f ? 0.5f : (s < 0.0005f ? 0.0005f : s);
  if (s > mag) s = mag;
  return diff < 0.0f ? cur - s : cur + s;
}

// The change between two effects that share one block of memory, as Reverb80 does between
// its own modes: with sound in the lines the output fades out over 20 ms, the lines are
// cleared a few KB per block in silence, then the other effect starts. With the lines known
// to be clear the change is immediate. All of it runs in the audio interrupt.
struct BankSwitch {
  int active = 0;      // the effect that runs
  bool clean = true;   // its lines are known silent
  bool fading = false, clearing = false;
  float gain = 1.0f;

  // Call first in each block with the wanted effect. Returns true if the block must be
  // silent and the active effect cleared by one step (then call cleared() when it is done).
  inline bool begin(int want) {
    if (want != active && !fading && !clearing) {
      if (clean) active = want;
      else fading = true;
    }
    return clearing;
  }
  inline void cleared(int want) {
    clearing = false;
    clean = true;
    active = want;
    gain = 1.0f;
  }
  // After the active effect has produced a block: applies the fade if one runs.
  inline void end(float* outL, float* outR, int n, float sampleRate) {
    clean = false;
    if (!fading) return;
    const float step = 1.0f / (0.02f * sampleRate);
    float g = gain;
    for (int i = 0; i < n; i++) {
      g = g > step ? g - step : 0.0f;
      outL[i] *= g;
      outR[i] *= g;
    }
    gain = g;
    if (g == 0.0f) {
      fading = false;
      clearing = true;
    }
  }
};

}  // namespace dsp
