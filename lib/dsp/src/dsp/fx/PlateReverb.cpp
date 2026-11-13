// PlateReverb.cpp - see PlateReverb.h. The setup, clearing and coefficient code lives here
// so it can run from flash (DSP_COLD); process() stays in the header.
#include <dsp/fx/PlateReverb.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void PlateReverb::init(float* mem) {
  float* p = mem;
  pre_ = p;
  p += kPreMax;
  for (int i = 0; i < 4; i++) {
    in_[i].bf = p;
    in_[i].len = kInLen[i];
    p += kInLen[i];
  }
  for (int h = 0; h < 2; h++)
    for (int e = 0; e < 4; e++) {
      tank_[h][e].bf = p;
      tank_[h][e].len = kTankLen[h][e] + (e == 0 ? kModPad : 0);
      p += tank_[h][e].len;
    }
  mem_ = mem;
  reset();
}

DSP_COLD bool PlateReverb::clearStep(int n) {
  if (!mem_) return true;
  if (clearPos_ == 0) {
    preIdx_ = 0;
    for (auto& r : in_) r.idx = 0;
    for (auto& h : tank_)
      for (auto& r : h) r.idx = 0;
    lp_[0] = lp_[1] = 0.0f;
    hpL_.clear();
    hpR_.clear();
  }
  const int count = n < kMemFloats - clearPos_ ? n : kMemFloats - clearPos_;
  memset(mem_ + clearPos_, 0, count * sizeof(float));
  clearPos_ += count;
  if (clearPos_ < kMemFloats) return false;
  clearPos_ = 0;
  return true;
}

DSP_COLD void PlateReverb::update() {
  cSize_ = size_;
  cTone_ = tone_;
  cMod_ = mod_;
  // One trip round both halves passes the decay gain four times.
  const float rt60 = kDecayTrim * 0.5f * powf(16.0f, size_);
  decay_ = powf(10.0f, -3.0f * (float)kTankFloats / (4.0f * rt60 * fs_));
  // The paper's "decay diffusion 2": decay + 0.15, kept between 0.25 and 0.5.
  const float d = decay_ + 0.15f;
  dd2_ = d < 0.25f ? 0.25f : (d > 0.5f ? 0.5f : d);
  dampK_ = onePoleK(2500.0f * powf(12000.0f / 2500.0f, tone_), fs_);
  modDepth_ = 12.0f * mod_;  // samples, times the LFO's 0..2
  hpL_.setHighpass(100.0f, fs_);
  hpR_.setHighpass(100.0f, fs_);
}

}  // namespace dsp
