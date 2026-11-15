// ResampledSource.cpp - see ResampledSource.h. All of it runs in the background, from
// flash (DSP_COLD).
#include <dsp/player/ResampledSource.h>

#include <math.h>
#include <string.h>

#include <dsp/Port.h>

namespace dsp {

namespace {

// The zeroth-order modified Bessel function, for the Kaiser window.
DSP_COLD double bessel0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

DSP_COLD bool ResampledSource::setSource(TrackSource* in) {
  in_ = nullptr;
  total_ = pos_ = 0;
  if (!in) return true;
  const double inRate = in->sampleRate(), outRate = outRate_;
  if (inRate * 6.0 < outRate || inRate > outRate * 2.2) return false;
  step_ = (uint64_t)(inRate / outRate * 4294967296.0 + 0.5);
  total_ = (uint32_t)((((uint64_t)in->frames() << 32) + step_ / 2) / step_);

  // The kernel, in input samples: a low-pass that ends at the lower of the two Nyquist
  // frequencies. The window's transition is 0.113 of the input rate wide; the cutoff sits
  // half of that below the edge.
  const double edge = outRate < inRate ? outRate / inRate : 1.0;  // of the input's Nyquist
  double fc = edge - 0.113;
  if (fc < 0.3 * edge) fc = 0.3 * edge;
  const double beta = 6.0, i0 = bessel0(beta), pi = 3.14159265358979323846;
  const double half = kTaps / 2.0;
  for (int p = 0; p <= kPhases; p++) {
    const double frac = (double)p / kPhases;
    double sum = 0.0;
    double h[kTaps];
    for (int j = 0; j < kTaps; j++) {
      const double t = (double)(j - (kTaps / 2 - 1)) - frac;  // from the output's place, in input samples
      const double r = t / half;
      const double w = fabs(r) < 1.0 ? bessel0(beta * sqrt(1.0 - r * r)) / i0 : 0.0;
      const double x = pi * fc * t;
      h[j] = (fabs(x) < 1e-9 ? 1.0 : sin(x) / x) * fc * w;
      sum += h[j];
    }
    for (int j = 0; j < kTaps; j++) m_.table[p * kTaps + j] = (float)(h[j] / sum);  // unity gain at DC
  }
  in_ = in;
  return seek(0);
}

// Makes the window start at input frame `first` (of the padded input) and fills it.
DSP_COLD void ResampledSource::slideTo(uint32_t first) {
  const uint32_t end = winStart_ + (uint32_t)winLen_;
  if (first >= winStart_ && first < end) {
    const int keep = (int)(end - first);
    memmove(m_.window, m_.window + 2 * (first - winStart_), sizeof(float) * 2 * (size_t)keep);
    winLen_ = keep;
  } else {
    winLen_ = 0;  // only after a seek, which has placed the source
  }
  winStart_ = first;
  while (winLen_ < kWindowFrames) {
    const uint32_t at = winStart_ + (uint32_t)winLen_;
    float* dst = m_.window + 2 * winLen_;
    const int room = kWindowFrames - winLen_;
    int got;
    if (at < (uint32_t)(kTaps / 2)) {
      got = kTaps / 2 - (int)at;  // the silence in front
      if (got > room) got = room;
      memset(dst, 0, sizeof(float) * 2 * (size_t)got);
    } else if (inEnded_) {
      got = room;  // the silence behind
      memset(dst, 0, sizeof(float) * 2 * (size_t)got);
    } else {
      const int want = room < kReadFrames ? room : kReadFrames;
      got = in_->read(m_.raw, want);
      if (got < want) inEnded_ = true;
      if (got < 0) got = 0;
      for (int i = 0; i < 2 * got; i++) dst[i] = (float)m_.raw[i];
    }
    winLen_ += got;
  }
}

DSP_COLD bool ResampledSource::seek(uint32_t frame) {
  if (!in_) return false;
  if (frame > total_) frame = total_;
  pos_ = frame;
  // Output frame k needs the padded input's frames n + 1 .. n + kTaps, n = floor(k x step);
  // padded frame i is the source's frame i - kTaps / 2.
  const uint32_t first = (uint32_t)(((uint64_t)frame * step_) >> 32) + 1;
  const uint32_t srcFrame = first > (uint32_t)(kTaps / 2) ? first - (uint32_t)(kTaps / 2) : 0;
  if (!in_->seek(srcFrame)) return false;
  inEnded_ = false;
  winStart_ = first;
  winLen_ = 0;
  // slideTo() fills from winStart_: the silence in front first, if `first` lies in it.
  return true;
}

DSP_COLD int ResampledSource::read(int16_t* dst, int maxFrames) {
  if (!in_ || maxFrames <= 0) return 0;
  const uint32_t left = total_ - pos_;
  const int want = (uint32_t)maxFrames < left ? maxFrames : (int)left;
  for (int i = 0; i < want; i++, pos_++) {
    const uint64_t at = (uint64_t)pos_ * step_;
    const uint32_t first = (uint32_t)(at >> 32) + 1;
    if (first < winStart_ || first + (uint32_t)kTaps > winStart_ + (uint32_t)winLen_) slideTo(first);
    const float pf = (float)(uint32_t)at * (kPhases / 4294967296.0f);
    int p = (int)pf;
    if (p >= kPhases) p = kPhases - 1;
    const float a = pf - (float)p;
    const float* t0 = m_.table + p * kTaps;
    const float* t1 = t0 + kTaps;
    const float* x = m_.window + 2 * (first - winStart_);
    float l = 0.0f, r = 0.0f;
    for (int j = 0; j < kTaps; j++) {
      const float c = t0[j] + a * (t1[j] - t0[j]);
      l += c * x[2 * j];
      r += c * x[2 * j + 1];
    }
    // Rounded by hand: libm's lrintf would be one more routine in RAM1.
    const long li = (long)(l + (l >= 0.0f ? 0.5f : -0.5f)), ri = (long)(r + (r >= 0.0f ? 0.5f : -0.5f));
    dst[2 * i] = (int16_t)(li > 32767 ? 32767 : (li < -32768 ? -32768 : li));
    dst[2 * i + 1] = (int16_t)(ri > 32767 ? 32767 : (ri < -32768 ? -32768 : ri));
  }
  return want;
}

}  // namespace dsp
