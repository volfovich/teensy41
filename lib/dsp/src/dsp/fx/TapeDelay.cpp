// TapeDelay.cpp - see TapeDelay.h. The setup, clearing and coefficient code lives here so
// it can run from flash (DSP_COLD); process() stays in the header.
#include <dsp/fx/TapeDelay.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void TapeDelay::init(float* line, int len) {
  a_ = line;
  len_ = len;
  setDelaySamples((float)maxDelay());
  reset();
}

DSP_COLD bool TapeDelay::clearStep(int n) {
  if (!a_) return true;
  if (clearPos_ == 0) {
    lpL_.clear();
    lpR_.clear();
    hpL_ = hpR_ = 0.0f;
  }
  const int count = n < len_ - clearPos_ ? n : len_ - clearPos_;
  memset(a_ + clearPos_, 0, count * sizeof(float));
  clearPos_ += count;
  if (clearPos_ < len_) return false;
  clearPos_ = 0;
  silent_ = true;
  return true;
}

DSP_COLD void TapeDelay::update() {
  cTone_ = tone_;
  const float hz = 1200.0f * powf(7000.0f / 1200.0f, tone_);
  lpL_.setLowpass(hz, fs_);
  lpR_.setLowpass(hz, fs_);
  hpK_ = onePoleK(110.0f, fs_);
}

}  // namespace dsp
