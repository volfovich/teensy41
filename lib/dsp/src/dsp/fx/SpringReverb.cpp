// SpringReverb.cpp - see SpringReverb.h. The setup, clearing and coefficient code lives
// here so it can run from flash (DSP_COLD); process() stays in the header.
#include <dsp/fx/SpringReverb.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void SpringReverb::init(float* mem) {
  float* p = mem;
  for (int s = 0; s < kSprings; s++) {
    line_[s].bf = p;
    line_[s].len = kLoopLen[s] + kPad;
    p += line_[s].len;
  }
  mem_ = mem;
  reset();
}

DSP_COLD bool SpringReverb::clearStep(int n) {
  if (!mem_) return true;
  if (clearPos_ == 0) {
    memset(hist_, 0, sizeof(hist_));
    inHp_.clear();
    for (int s = 0; s < kSprings; s++) {
      line_[s].idx = 0;
      loopLp_[s] = dc_[s] = 0.0f;
      lpA_[s].clear();
      lpB_[s].clear();
    }
  }
  const int count = n < kMemFloats - clearPos_ ? n : kMemFloats - clearPos_;
  memset(mem_ + clearPos_, 0, count * sizeof(float));
  clearPos_ += count;
  if (clearPos_ < kMemFloats) return false;
  clearPos_ = 0;
  return true;
}

DSP_COLD void SpringReverb::update() {
  cSize_ = size_;
  cTone_ = tone_;
  const float rt60 = kDecayTrim * 0.8f * powf(4.0f, size_);
  for (int s = 0; s < kSprings; s++) {
    // One pass: the delay plus the all-passes' delay at low frequencies. The sound takes
    // both springs' passes in turn, losing 60 dB in the decay time.
    const float a = kStageGain[s];
    const float trip = (float)kLoopLen[s] + (float)(kStages * kStretch) * (1.0f - a) / (1.0f + a);
    gain_[s] = powf(10.0f, -3.0f * trip / (rt60 * fs_));
  }
  const float top = 2000.0f * powf(3800.0f / 2000.0f, tone_);
  for (int s = 0; s < kSprings; s++) {
    lpA_[s].setLowpass(top, fs_, 0.54f);  // fourth-order Butterworth
    lpB_[s].setLowpass(top, fs_, 1.31f);
  }
  dampK_ = onePoleK(5500.0f, fs_);
  dcK_ = onePoleK(40.0f, fs_);
  inHp_.setHighpass(150.0f, fs_);
}

}  // namespace dsp
