// ResampledSource.h - a TrackSource at another sample rate: wraps a source (a 44.1 kHz MP3,
// say) and gives its sound at the output's rate (48 kHz), a piece at a time, with a seek.
//
//   out[k] = sum over 32 input frames around k x (in rate / out rate) of in[n] x h(...)
//
//   - h is a windowed sinc (Kaiser, beta 6: about -60 dB), 32 taps, kept in a table of 64
//     positions between two input samples and interpolated between those.
//   - Going up from 44.1 kHz the response is flat to about 17 kHz and closed at 22 kHz.
//     Going down (a 96 kHz file) the same 32 taps cover less: flat to about 13 kHz.
//   - Positions and lengths are in output frames. A seek gives the very samples a read from
//     the start gives.
//   - This is not the IR loader's resampler (dsp/conv/IRLoader.h): that one works on a whole
//     mono file at once and scales its gain as an impulse response needs.
//
// Memory is handed in (see Memory). Background only.
#pragma once

#include <stdint.h>

#include <dsp/player/TrackSource.h>

namespace dsp {

class ResampledSource : public TrackSource {
 public:
  static constexpr int kTaps = 32;
  static constexpr int kPhases = 64;
  static constexpr int kTableFloats = (kPhases + 1) * kTaps;
  static constexpr int kWindowFrames = 1024;  // input frames held at a time
  static constexpr int kReadFrames = 256;     // the source is read this many frames at a time

  struct Memory {
    float* table;   // kTableFloats
    float* window;  // 2 * kWindowFrames: the input as floats (converted once, used 32 times)
    int16_t* raw;   // 2 * kReadFrames: the source is read through here
  };

  ResampledSource(const Memory& m, uint32_t outRate) : m_(m), outRate_(outRate) {}

  // The source to resample (nullptr: none). False if its rate is below a sixth or above
  // 2.2 times the output's.
  bool setSource(TrackSource* in);

  uint32_t sampleRate() const override { return outRate_; }
  uint32_t frames() const override { return total_; }
  int read(int16_t* dst, int maxFrames) override;
  bool seek(uint32_t frame) override;

 private:
  void slideTo(uint32_t first);

  Memory m_;
  uint32_t outRate_;
  TrackSource* in_ = nullptr;
  uint64_t step_ = 0;       // input frames per output frame, 32.32
  uint32_t total_ = 0, pos_ = 0;
  // The input as the filter sees it: kTaps / 2 silent frames, the source, then silence.
  // The window holds frames [winStart_, winStart_ + winLen_) of that.
  uint32_t winStart_ = 0;
  int winLen_ = 0;
  bool inEnded_ = false;
};

}  // namespace dsp
