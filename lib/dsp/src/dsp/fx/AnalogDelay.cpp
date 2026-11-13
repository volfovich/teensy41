// AnalogDelay.cpp - see AnalogDelay.h. The setup, clearing and coefficient code lives here
// so it can run from flash (DSP_COLD); process() stays in the header.
#include <dsp/fx/AnalogDelay.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void AnalogDelay::init(float* line, int len) {
  a_ = line;
  len_ = len;
  setDelaySamples((float)maxDelay());
  reset();
}

DSP_COLD bool AnalogDelay::clearStep(int n) {
  if (!a_) return true;
  if (clearPos_ == 0) {
    inLp_.clear();
    outLp_.clear();
    pole_ = last_ = envC_ = envE_ = 0.0f;
  }
  const int count = n < len_ - clearPos_ ? n : len_ - clearPos_;
  memset(a_ + clearPos_, 0, count * sizeof(float));
  clearPos_ += count;
  if (clearPos_ < len_) return false;
  clearPos_ = 0;
  silent_ = true;
  return true;
}

DSP_COLD void AnalogDelay::update() {
  cTone_ = tone_;
  cTime_ = dSet_;
  // The clock slows for long times and the filters follow: full width up to 300 ms, 30 %
  // lower at 750 ms.
  const float ms = dSet_ * 1000.0f / fs_;
  float slow = 1.0f - 0.3f * (ms - 300.0f) / 450.0f;
  slow = slow > 1.0f ? 1.0f : (slow < 0.7f ? 0.7f : slow);
  const float hz = slow * 1500.0f * powf(3.0f, tone_);
  inLp_.setLowpass(hz, fs_);
  outLp_.setLowpass(hz, fs_);
  poleK_ = onePoleK(1.5f * hz, fs_);
}

}  // namespace dsp
