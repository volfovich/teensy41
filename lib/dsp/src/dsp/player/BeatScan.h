// BeatScan.h - finds a backing track's tempo and where its beats fall, in one pass over its
// audio.
//
// The model is a track played to a steady click: one tempo, beats at firstBeat + k x
// 60 / bpm. That is what the result says, to a precision that holds over the whole track
// (a few ms after several minutes). A track whose tempo wanders gets a low confidence and
// a grid that only fits in places.
//
//   audio -> three bands (below 200 Hz, 200 Hz to 2 kHz, above 4 kHz)
//         -> each band's level 200 times a second (over the last 20 ms, so that two notes
//            beating against each other do not look like hits), as a logarithm
//         -> how much each rose above its highest reading of the 40 ms before (so a level
//            that only ripples, as chords do, counts for nothing), summed  = onset strength
//   while the audio passes:
//     - the onset strength's autocorrelation, for periods up to 2 s
//     - its peaks (the onsets), with their time and strength
//   at the end:
//     - the beat period: the strongest autocorrelation between 60 and 200 BPM, counting a
//       period whose double is strong too, with a preference for tempos near 120 BPM
//     - the exact tempo: the frequency near that at which the onsets, as a train of
//       pulses, have the most energy (searched within 3 %, finer than their spacing)
//     - the first beat: from the phase of that frequency
//     - the confidence: how much of the onsets' strength lies on the beats, or on the
//       beats and the eighths between them (0 to 1). In the host tests a steady band is
//       above 0.6 and one whose tempo swings by 10 % is below 0.5.
//
//   - Twice or half the tempo is a possible answer (a slow song counted in eighths): the
//     caller offers halving and doubling.
//   - Which beat starts a bar is not found.
//
// Memory is handed in. Background only; feed() takes any sample rate from 8 kHz up.
#pragma once

#include <stdint.h>

#include <dsp/Blocks.h>

namespace dsp {

class BeatScan {
 public:
  static constexpr int kEnvRate = 200;    // onset-strength readings per second
  static constexpr int kMaxPeaks = 4096;  // onsets kept (about 8 minutes of a busy track)
  static constexpr float kMinBpm = 60.0f, kMaxBpm = 200.0f;
  static constexpr int kRing = 512;       // readings held for the autocorrelation
  static constexpr int kMaxLag = 2 * kEnvRate * 60 / 60;  // 400: two beats at 60 BPM

  struct Peak {
    float t;  // seconds
    float w;  // strength
  };
  struct Memory {
    Peak* peaks;  // kMaxPeaks
  };
  struct Result {
    bool found = false;     // false: too few onsets, or nothing like a beat (confidence under 0.2)
    float bpm = 0.0f;
    float firstBeat = 0.0f;  // seconds; the beats are firstBeat + k x 60 / bpm
    float confidence = 0.0f;
  };

  explicit BeatScan(const Memory& m) : m_(m) {}

  void begin(uint32_t sampleRate);
  void feed(const int16_t* stereo, int frames);
  Result finish();
  uint32_t fed() const { return fed_; }  // frames so far

 private:
  void reading(float v);

  Memory m_;
  float rate_ = 48000.0f;
  uint32_t fed_ = 0;
  Biquad low_, midHp_, midLp_, high_;
  static constexpr int kBefore = 8;  // readings a rise is measured against (40 ms)
  float sum_[3] = {0, 0, 0};
  float levels_[3][kBefore] = {};    // each band's last readings
  int levelAt_ = 0;
  float hops_[3][4] = {};  // each band's energy in the last four readings' time
  double hopLeft_ = 0.0, hopLen_ = 240.0;
  int hopCount_ = 0;
  uint32_t readings_ = 0;
  float ring_[kRing];
  float acf_[kMaxLag + 1];
  float avg_ = 0.0f;  // the onset strength's running average (1 s)
  int peakCount_ = 0;
};

}  // namespace dsp
