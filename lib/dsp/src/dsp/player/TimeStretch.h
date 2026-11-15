// TimeStretch.h - plays a backing track slower or faster without changing its pitch.
//
// The method is a phase vocoder with locked phases (Laroche and Dolson, "Improved phase
// vocoder time-scale modification of audio", 1999) and a phase reset at drum hits (as the
// Rubber Band library does):
//
//   - The track is taken in windows of 2048 frames (43 ms). Each window is turned into
//     its spectrum, and the windows are laid down again 512 frames apart in the output
//     while they are taken (512 x rate) frames apart from the input: closer together to
//     slow down, further apart to speed up.
//   - So that each steady tone goes on smoothly from one output window to the next, its
//     phase is turned by what the tone's own frequency asks for over 512 frames. The
//     tones are the peaks of the spectrum; the frequency of each is read from how far its
//     phase moved between the last two windows.
//   - All bins around a peak are turned by the same angle as the peak ("identity phase
//     locking"): they keep the phase relations they had in the input, which is what
//     keeps the sound from going hollow and washy as a plain phase vocoder's does.
//   - A drum hit must not be stretched: it would turn into a smear. A window in which
//     the level rises suddenly in much of the spectrum, and the next four (until the hit
//     has passed through), are taken from the input as far apart as they are laid down,
//     with the phases they have ("phase reset"): the hit comes out exactly as it is. The
//     time is made up after the hit, where the windows move up to half as fast again (or
//     half as slow) until the tempo's pace is caught up.
//   - Both sides are turned by the same angles, found on a mix of the two: the stereo
//     image stays. Sum and difference go through one complex FFT together (the sum as the
//     real part, the difference as the imaginary part), so a window costs one transform
//     forth and one back.
//   - At a rate of exactly 1 the caller should not use it at all (TrackPlayer does not):
//     the result would be the input within rounding, but not bit for bit.
//   - push() what the source gives, pull() what is ready; finish() when the source has
//     ended.
//
// Before this (both 15 Nov 2026) the method was WSOLA, pieces of the track cut and
// joined in the time domain: first 40 ms pieces overlapping by half, then 60 ms pieces
// with 10 ms joins. The author heard the joins on real songs as nasty artifacts and as
// ticking, even at a change of 1 %. The phase vocoder has no joins.
//
// Memory is handed in (see Memory). Background only.
#pragma once

#include <stdint.h>

namespace dsp {

class TimeStretch {
 public:
  static constexpr int kN = 2048;      // a window: 43 ms at 48 kHz
  static constexpr int kHop = kN / 4;  // the output a window gives: 512 frames
  static constexpr int kInFrames = 4096;  // input held (stereo)
  static constexpr int kMaxPeaks = 340;   // peaks are 3 bins apart or more
  static constexpr int kBins = kN / 2 + 1;
  // The work memory: the transform's buffer and its table, the last window's spectrum
  // (the sides' sum), this window's power, what is laid down and not yet out, and the
  // turns of this window's and the last window's peaks.
  static constexpr int kWorkFloats = 2 * kN + kN + 2 * kBins + kBins + 2 * (kN - kHop) + 4 * kMaxPeaks;
  static constexpr int kIndexWords = 2 * kMaxPeaks + kBins;
  static constexpr float kMinRate = 0.5f, kMaxRate = 1.5f;

  struct Memory {
    int16_t* in;      // 2 * kInFrames; read through once per window (slow memory will do)
    float* work;      // kWorkFloats; worked on all the time, so in fast memory
    uint16_t* index;  // kIndexWords; the same
    int16_t* out;     // 2 * kHop; the same
  };

  explicit TimeStretch(const Memory& m);

  // The speed: 0.5 = half, 1.5 = one and a half times. Takes effect with the next window.
  void setRate(float rate);
  float rate() const { return rate_; }
  // Forgets everything: the next push() is the start of a new stream.
  void reset();

  // Frames push() can take. Input no later window can reach makes room.
  int room() const { return kInFrames - (int)(fed_ - (keepFrom_ > base_ ? keepFrom_ : base_)); }
  void push(const int16_t* in, int frames);
  void finish() { finished_ = true; }  // no more input: the rest comes out
  // Up to maxFrames frames of output; fewer (or 0) when more input is needed first, or,
  // after finish(), when everything is out.
  int pull(int16_t* out, int maxFrames);
  // (The window whose middle is that far past the input's end is the first that adds
  // nothing to the output up to the end.)
  bool done() const {
    return finished_ && outLeft_ == 0 && centre_ >= (double)fed_ + (double)(kN / 2 - kHop) * (double)rate_;
  }
  // How many input frames have been pushed that the output has not reached yet.
  float lag() const {
    const double d = (double)fed_ - (outAt_ + (double)(kHop - outLeft_) * (double)rate_);
    return d > 0.0 ? (float)d : 0.0f;
  }

 private:
  bool makeWindow();
  void forward();
  void mixAt(int k, float* r, float* i) const;
  void inverse();
  int16_t sampleAt(int64_t i, int ch) const {
    return i >= (int64_t)base_ && i < (int64_t)fed_ ? m_.in[2 * (i - (int64_t)base_) + ch] : 0;
  }

  Memory m_;
  float* re_;      // kN: the window, then its spectrum (bins in bit-reversed order)
  float* im_;      // kN
  float* cos_;     // kN / 2: cos(2 pi i / kN); the window's shape comes from it too
  float* sin_;     // kN / 2
  float* lastRe_;  // kBins: the last window's spectrum (of the mix the phases are read from)
  float* lastIm_;  // kBins
  float* power_;   // kBins: this window's
  float* accM_;    // kN - kHop: laid down and not yet out (the sum)
  float* accS_;    // kN - kHop: the same (the difference)
  float* turn_;    // 2 * kMaxPeaks: this window's peaks' turns (re, im)
  float* lastTurn_;      // 2 * kMaxPeaks: the last window's
  uint16_t* peak_;       // kMaxPeaks: this window's peaks' bins
  uint16_t* lastPeak_;   // kMaxPeaks: the last window's
  uint16_t* lastOwner_;  // kBins: which of the last window's peaks each bin went with
  float rate_ = 1.0f;
  uint64_t base_ = 0, fed_ = 0;  // the input held is frames [base_, fed_) of the stream
  uint64_t keepFrom_ = 0;        // frames before this are no longer needed
  double centre_ = 0.0;          // the middle of the next window, in the input
  int64_t lastStart_ = 0;        // where the last window started
  int windows_ = 0;              // made since reset()
  int lastPeaks_ = 0;
  float lastRising_ = 0.0f;      // how much of the last window's spectrum rose
  int holdLeft_ = 0;             // windows of a hit still to come (this one counted)
  double owed_ = 0.0;            // input frames the windows are ahead of the tempo's pace
  int outLeft_ = 0;              // frames in out not yet pulled
  double outAt_ = 0.0;           // where in the input those frames begin
  bool finished_ = false;
};

}  // namespace dsp
