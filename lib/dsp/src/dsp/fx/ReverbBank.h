// ReverbBank.h - the Reverb block's four modes behind one interface: Reverb80's hall and
// gated (ported, untouched), PlateReverb and SpringReverb. The three effects share one block
// of memory, so only one of them holds sound at a time.
//
//   - Hall and gated go straight to Reverb80, which changes between its two modes itself:
//     in those modes the samples are exactly Reverb80's.
//   - A change to or from plate or spring does what Reverb80 does between its own modes:
//     fade the tail out (20 ms), clear the lines a few KB per block in silence, then start
//     the other effect. With the lines already clear the change is immediate.
//   - The settings go to every effect that has them, so they are in place when the mode
//     changes. Plate has no Era and no Gate; spring has Size and Tone only.
//
// Threading model: process() and clearStep() run in the audio interrupt; the setters only
// write single values and may be called from the background.
#pragma once

#include <dsp/fx/FxParts.h>
#include <dsp/fx/PlateReverb.h>
#include <dsp/fx/Reverb80.h>
#include <dsp/fx/SpringReverb.h>

namespace dsp {

class ReverbBank {
 public:
  enum Mode : int { kHall = 0, kGated = 1, kPlate = 2, kSpring = 3, kModeCount };

  // Total buffer size for init(), in floats: Reverb80's, which the others fit into.
  static constexpr int kMemFloats = Reverb80::kMemFloats;
  static_assert(PlateReverb::kMemFloats <= kMemFloats, "the plate must fit the shared memory");
  static_assert(SpringReverb::kMemFloats <= kMemFloats, "the spring must fit the shared memory");

  explicit ReverbBank(float sampleRate) : fs_(sampleRate), r80_(sampleRate), plate_(sampleRate), spring_(sampleRate) {}

  // Hands over the line memory (kMemFloats floats) and clears it.
  void init(float* mem) {
    plate_.init(mem);
    spring_.init(mem);
    r80_.init(mem);
  }
  bool ready() const { return r80_.ready(); }

  // Clears up to n floats per call; returns true when the reverb is silent.
  bool clearStep(int n) {
    if (!clearActive(n)) return false;
    sw_.clean = true;
    return true;
  }

  void setMode(int m) {
    m = m < 0 || m >= kModeCount ? kHall : m;
    if (m <= kGated) r80_.setMode(m);
    modeReq_ = m;
  }
  int mode() const { return modeReq_; }
  void setSize(float n) {
    r80_.setSize(n);
    plate_.setSize(n);
    spring_.setSize(n);
  }
  void setTone(float n) {
    r80_.setTone(n);
    plate_.setTone(n);
    spring_.setTone(n);
  }
  void setPreDelayMs(float ms) {
    r80_.setPreDelayMs(ms);
    plate_.setPreDelayMs(ms);
  }
  void setMod(float n) {
    r80_.setMod(n);
    plate_.setMod(n);
  }
  void setEra(float n) { r80_.setEra(n); }
  void setGateMs(float ms) { r80_.setGateMs(ms); }

  // Processes n samples into the wet-only stereo outputs.
  void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    if (sw_.begin(effectOf(modeReq_))) {
      for (int i = 0; i < n; i++) outL[i] = outR[i] = 0.0f;
      if (clearActive(kSwitchClear)) sw_.cleared(effectOf(modeReq_));
      return;
    }
    if (sw_.active == kFxPlate) plate_.process(inL, inR, outL, outR, n);
    else if (sw_.active == kFxSpring) spring_.process(inL, inR, outL, outR, n);
    else r80_.process(inL, inR, outL, outR, n);
    sw_.end(outL, outR, n, fs_);
  }

 private:
  enum Effect : int { kFx80 = 0, kFxPlate, kFxSpring };
  static constexpr int kSwitchClear = 4096;  // floats cleared per block in a mode change

  static int effectOf(int mode) { return mode == kPlate ? kFxPlate : (mode == kSpring ? kFxSpring : kFx80); }
  bool clearActive(int n) {
    if (sw_.active == kFxPlate) return plate_.clearStep(n);
    if (sw_.active == kFxSpring) return spring_.clearStep(n);
    return r80_.clearStep(n);
  }

  const float fs_;
  Reverb80 r80_;
  PlateReverb plate_;
  SpringReverb spring_;
  BankSwitch sw_;  // audio interrupt only
  volatile int modeReq_ = kHall;
};

}  // namespace dsp
