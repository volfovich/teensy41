// TrackOut.h - the backing track's side of the audio interrupt: takes one block from the
// queue (StereoRing.h), converts it to float and applies the track's volume. Everything
// else about a track (reading the file, decoding, looping) happens in the background
// (TrackPlayer.h).
//
//   - Starting and stopping fade over 10 ms, so neither clicks.
//   - While stopped and faded out, process() does nothing and returns false: the chain
//     then runs exactly as it does without a player.
//   - A block the queue cannot fill is completed with silence and counted (underruns()),
//     unless the writer said the track is ending (setEnding()).
//
// Threading model: the setters run in the background, process() in the audio interrupt.
#pragma once

#include <stdint.h>
#include <string.h>

#include <dsp/player/StereoRing.h>

namespace dsp {

class TrackOut {
 public:
  static constexpr int kMaxBlock = 128;  // frames per process() call

  explicit TrackOut(float sampleRate) : step_(1.0f / (sampleRate * 0.010f)) {}

  void begin(StereoRing* ring) { ring_ = ring; }

  void setRun(bool on) { run_ = on; }
  bool running() const { return run_; }
  // Stopped and faded out: the interrupt no longer reads the queue.
  bool idle() const { return !run_ && done_; }
  void setGain(float g) { gain_ = g; }
  void setEnding(bool on) { ending_ = on; }
  uint32_t underruns() const { return underruns_; }
  void clearUnderruns() { underruns_ = 0; }

  // One block of n (<= kMaxBlock) frames into l and r; false (and nothing written) while idle.
  bool process(float* l, float* r, int n) {
    const bool run = run_;
    if (!ring_ || (!run && cur_ <= 0.0f)) {
      done_ = true;
      return false;
    }
    done_ = false;
    int16_t buf[2 * kMaxBlock];
    const int got = (int)ring_->read(buf, (uint32_t)n);
    if (got < n) {
      if (run && !ending_) underruns_ = underruns_ + 1;
      memset(buf + 2 * got, 0, (size_t)(n - got) * 2 * sizeof(int16_t));
    }
    const float target = run ? gain_ : 0.0f;
    const float g0 = cur_;
    const float span = step_ * (float)n;
    const float g1 = target > g0 ? (g0 + span < target ? g0 + span : target) : (g0 - span > target ? g0 - span : target);
    const float dg = (g1 - g0) / (float)n;
    constexpr float kScale = 1.0f / 32768.0f;
    for (int i = 0; i < n; i++) {
      const float g = (g0 + dg * (float)(i + 1)) * kScale;
      l[i] = (float)buf[2 * i] * g;
      r[i] = (float)buf[2 * i + 1] * g;
    }
    cur_ = g1;
    return true;
  }

 private:
  StereoRing* ring_ = nullptr;
  float step_;
  float cur_ = 0.0f;  // the gain now (the interrupt's)
  volatile float gain_ = 1.0f;
  volatile bool run_ = false, done_ = true, ending_ = false;
  volatile uint32_t underruns_ = 0;
};

}  // namespace dsp
