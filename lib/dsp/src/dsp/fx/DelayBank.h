// DelayBank.h - the Delay block's three types behind one interface: StudioDelay (digital;
// ported, untouched), TapeDelay and AnalogDelay. They share the two delay lines (tape and
// analog use the first one only), so only one of them holds sound at a time.
//
//   - With the digital type the samples are exactly StudioDelay's.
//   - A change of type fades the repeats out (20 ms), clears the lines a few KB per block in
//     silence, then starts the other effect; with the lines already clear it is immediate.
//   - The settings go to every effect that has them. Tape has no Duck and no Era; analog has
//     no Duck, Drive or Era either.
//
// Threading model: process() and clearStep() run in the audio interrupt; the setters only
// write a few values each and may be called from the background.
#pragma once

#include <dsp/fx/AnalogDelay.h>
#include <dsp/fx/FxParts.h>
#include <dsp/fx/StudioDelay.h>
#include <dsp/fx/TapeDelay.h>

namespace dsp {

class DelayBank {
 public:
  enum Type : int { kDigital = 0, kTape = 1, kAnalog = 2, kTypeCount };

  explicit DelayBank(float sampleRate) : fs_(sampleRate), studio_(sampleRate), tape_(sampleRate), analog_(sampleRate) {}

  // Hands over the two line buffers (len floats each) and clears them.
  void init(float* lineA, float* lineB, int len) {
    tape_.init(lineA, len);
    analog_.init(lineA, len);
    studio_.init(lineA, lineB, len);
  }
  bool ready() const { return studio_.ready(); }
  // The digital delay, for its fixed voicing (right-side ratio, cross-feed, ducking times).
  StudioDelay& studio() { return studio_; }

  // Clears up to n samples of the lines per call; returns true when the delay is silent.
  bool clearStep(int n) {
    if (!clearActive(n)) return false;
    sw_.clean = true;
    return true;
  }

  void setType(int t) { typeReq_ = t < 0 || t >= kTypeCount ? kDigital : t; }
  int type() const { return typeReq_; }
  void setDelaySamples(float d) {
    studio_.setDelaySamples(d);
    tape_.setDelaySamples(d);
    analog_.setDelaySamples(d);
  }
  void setFeedback(float n) {
    studio_.setFeedback(n);
    tape_.setFeedback(n);
    analog_.setFeedback(n);
  }
  void setTone(float n) {
    studio_.setTone(n);
    tape_.setTone(n);
    analog_.setTone(n);
  }
  void setModDepth(float n) {
    studio_.setModDepth(n);
    tape_.setModDepth(n);
    analog_.setModDepth(n);
  }
  void setDrive(float n) {
    studio_.setDrive(n);
    tape_.setDrive(n);
  }
  void setDuck(float n) { studio_.setDuck(n); }
  void setEra(float n) { studio_.setEra(n); }

  // Processes n samples of mono input into the wet-only stereo outputs.
  void process(const float* in, float* outL, float* outR, int n) {
    if (sw_.begin(typeReq_)) {
      for (int i = 0; i < n; i++) outL[i] = outR[i] = 0.0f;
      if (clearActive(kSwitchClear)) sw_.cleared(typeReq_);
      return;
    }
    if (sw_.active == kTape) tape_.process(in, outL, outR, n);
    else if (sw_.active == kAnalog) analog_.process(in, outL, outR, n);
    else studio_.process(in, outL, outR, n);
    sw_.end(outL, outR, n, fs_);
  }

 private:
  static constexpr int kSwitchClear = 4096;  // samples cleared per block in a type change

  bool clearActive(int n) {
    if (sw_.active == kTape) return tape_.clearStep(n);
    if (sw_.active == kAnalog) return analog_.clearStep(n);
    return studio_.clearStep(n);
  }

  const float fs_;
  StudioDelay studio_;
  TapeDelay tape_;
  AnalogDelay analog_;
  BankSwitch sw_;  // audio interrupt only
  volatile int typeReq_ = kDigital;
};

}  // namespace dsp
