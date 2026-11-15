// BeatScan.cpp - see BeatScan.h. All of it runs in the background, from flash (DSP_COLD).
#include <dsp/player/BeatScan.h>

#include <math.h>
#include <string.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void BeatScan::begin(uint32_t sampleRate) {
  rate_ = (float)sampleRate;
  fed_ = 0;
  low_.clear();
  midHp_.clear();
  midLp_.clear();
  high_.clear();
  low_.setLowpass(200.0f, rate_);
  midHp_.setHighpass(200.0f, rate_);
  midLp_.setLowpass(2000.0f, rate_);
  high_.setHighpass(rate_ > 10000.0f ? 4000.0f : 0.4f * rate_, rate_);
  for (int b = 0; b < 3; b++) sum_[b] = 0.0f;
  memset(hops_, 0, sizeof(hops_));
  memset(levels_, 0, sizeof(levels_));
  levelAt_ = 0;
  hopLen_ = (double)sampleRate / kEnvRate;
  hopLeft_ = hopLen_;
  hopCount_ = 0;
  readings_ = 0;
  memset(ring_, 0, sizeof(ring_));
  memset(acf_, 0, sizeof(acf_));
  avg_ = 0.0f;
  peakCount_ = 0;
}

DSP_COLD void BeatScan::feed(const int16_t* stereo, int frames) {
  constexpr float kScale = 1.0f / 65536.0f;
  for (int i = 0; i < frames; i++) {
    const float x = (float)(stereo[2 * i] + stereo[2 * i + 1]) * kScale;
    const float lo = low_.process(x);
    const float mid = midLp_.process(midHp_.process(x));
    const float hi = high_.process(x);
    sum_[0] += lo * lo;
    sum_[1] += mid * mid;
    sum_[2] += hi * hi;
    hopCount_++;
    hopLeft_ -= 1.0;
    if (hopLeft_ > 0.0) continue;
    hopLeft_ += hopLen_;
    // Each band's level as a logarithm; how much it rose above the readings before.
    static const float kWeight[3] = {1.0f, 0.7f, 0.7f};
    float v = 0.0f;
    for (int b = 0; b < 3; b++) {
      float* e = hops_[b];
      e[3] = e[2];
      e[2] = e[1];
      e[1] = e[0];
      e[0] = sum_[b] / (float)hopCount_;
      const float level = logf(1.0f + 10000.0f * 0.25f * (e[0] + e[1] + e[2] + e[3]));
      float before = 0.0f;
      for (int k = 0; k < kBefore; k++) before = levels_[b][k] > before ? levels_[b][k] : before;
      if (level > before) v += kWeight[b] * (level - before);
      levels_[b][levelAt_] = level;
      sum_[b] = 0.0f;
    }
    levelAt_ = (levelAt_ + 1) % kBefore;
    hopCount_ = 0;
    reading(v);
  }
  fed_ += (uint32_t)frames;
}

// One reading of the onset strength (every 1 / kEnvRate s).
DSP_COLD void BeatScan::reading(float v) {
  const uint32_t n = readings_++;
  ring_[n & (kRing - 1)] = v;
  // The autocorrelation, as it comes.
  if (v > 0.0f) {
    const uint32_t lags = n < (uint32_t)kMaxLag ? n : (uint32_t)kMaxLag;
    for (uint32_t lag = 1; lag <= lags; lag++) acf_[lag] += v * ring_[(n - lag) & (kRing - 1)];
  }
  avg_ += (v - avg_) * (1.0f / kEnvRate);
  // A peak: the reading three back, if none of the three on either side is higher and it
  // stands out of the average.
  if (n < 6) return;
  const uint32_t c = n - 3;
  const float p = ring_[c & (kRing - 1)];
  if (p < 2.0f * avg_ + 0.05f) return;
  for (uint32_t k = c - 3; k <= c + 3; k++) {
    if (k == c) continue;
    const float o = ring_[k & (kRing - 1)];
    if (o > p || (o == p && k < c)) return;
  }
  if (peakCount_ >= kMaxPeaks) return;
  // Between the readings: where a parabola through the three highest peaks.
  const float a = ring_[(c - 1) & (kRing - 1)], b = ring_[(c + 1) & (kRing - 1)];
  const float d = a - 2.0f * p + b;
  const float shift = d < 0.0f ? 0.5f * (a - b) / d : 0.0f;
  // A reading covers the 1 / kEnvRate s before it; the rise is at its middle.
  m_.peaks[peakCount_].t = ((float)c + 0.5f + shift) / kEnvRate;
  m_.peaks[peakCount_].w = p;
  peakCount_++;
}

DSP_COLD BeatScan::Result BeatScan::finish() {
  Result r;
  if (peakCount_ < 16 || readings_ < (uint32_t)(4 * kEnvRate)) return r;

  // The beat period from the autocorrelation: between 200 and 60 BPM, each lag counted
  // with its double, and tempos near 120 BPM preferred (a bell over the octaves).
  const int lagMin = (int)(kEnvRate * 60.0f / kMaxBpm), lagMax = (int)(kEnvRate * 60.0f / kMinBpm);
  float best = 0.0f;
  int bestLag = 0;
  for (int lag = lagMin; lag <= lagMax; lag++) {
    // (The sums ran over fewer pairs for longer lags.)
    const float one = acf_[lag] / (float)(readings_ - (uint32_t)lag);
    const float two = acf_[2 * lag] / (float)(readings_ - (uint32_t)(2 * lag));
    const float octaves = log2f((60.0f * kEnvRate / (float)lag) / 120.0f);
    const float score = (one + 0.5f * two) * expf(-0.5f * (octaves / 0.9f) * (octaves / 0.9f));
    if (score > best) {
      best = score;
      bestLag = lag;
    }
  }
  if (bestLag == 0) return r;

  // The exact tempo: the onsets as pulses at their times; near the period found, the
  // frequency at which they add up most. Steps of a quarter of what the track's length
  // can tell apart.
  const float span = m_.peaks[peakCount_ - 1].t - m_.peaks[0].t;
  if (span < 4.0f) return r;
  const float f0 = (float)kEnvRate / (float)bestLag;
  const float step = 0.25f / span;
  int steps = (int)(0.03f * f0 / step) + 1;
  if (steps > 600) steps = 600;
  float total = 0.0f, squares = 0.0f;
  for (int i = 0; i < peakCount_; i++) {
    total += m_.peaks[i].w;
    squares += m_.peaks[i].w * m_.peaks[i].w;
  }
  if (total <= 0.0f) return r;
  constexpr float kPi2 = 6.2831853f;
  // At each frequency f: how much the onsets add up at f (the beats against what lies
  // between them) and at 2 f (beats and the eighths between them together). Music with
  // eighths as strong as its beats shows at 2 f only, so the two are added.
  float bestMag = -1.0f, bestF = f0;
  float mags[3] = {0, 0, 0};  // the best and its two neighbours
  float prev = 0.0f;
  int bestAt = -steps - 1;
  for (int s = -steps; s <= steps + 1; s++) {
    float mag = 0.0f;
    if (s <= steps) {
      const float f = f0 + (float)s * step;
      float re1 = 0.0f, im1 = 0.0f, re2 = 0.0f, im2 = 0.0f;
      for (int i = 0; i < peakCount_; i++) {
        // The phase as a fraction of a turn first, so that long times keep their precision.
        const double turns = (double)f * (double)m_.peaks[i].t;
        const float ph = kPi2 * (float)(turns - floor(turns));
        const float co = cosf(ph), si = sinf(ph), w = m_.peaks[i].w;
        re1 += w * co;
        im1 += w * si;
        re2 += w * (co * co - si * si);
        im2 += w * 2.0f * si * co;
      }
      mag = sqrtf(re1 * re1 + im1 * im1) + sqrtf(re2 * re2 + im2 * im2);
    }
    if (bestAt == s - 1) mags[2] = mag;
    if (s <= steps && mag > bestMag) {
      bestMag = mag;
      bestF = f0 + (float)s * step;
      bestAt = s;
      mags[0] = prev;
      mags[1] = mag;
      mags[2] = 0.0f;
    }
    prev = mag;
  }
  // Between the steps: where a parabola through the best three peaks.
  double f = bestF;
  const float curve = mags[0] - 2.0f * mags[1] + mags[2];
  if (bestAt > -steps && bestAt < steps && curve < 0.0f) f += 0.5 * (double)(mags[0] - mags[2]) / (double)curve * (double)step;

  // The beats' phase at that tempo: the onsets gather at t = phase (mod period).
  constexpr double kTurn = 6.283185307179586;
  double re1 = 0.0, im1 = 0.0, re2 = 0.0, im2 = 0.0;
  for (int i = 0; i < peakCount_; i++) {
    const double turns = f * (double)m_.peaks[i].t;
    const double ph = kTurn * (turns - floor(turns));
    const double w = m_.peaks[i].w;
    re1 += w * cos(ph);
    im1 += w * sin(ph);
    re2 += w * cos(2.0 * ph);
    im2 += w * sin(2.0 * ph);
  }
  const double period = 1.0 / f;
  const double mag1 = sqrt(re1 * re1 + im1 * im1), mag2 = sqrt(re2 * re2 + im2 * im2);
  double first;
  if (mag2 > mag1) {
    // From the eighths, which are the stronger: that leaves two places half a beat apart;
    // the one the beats lean to is taken.
    first = atan2(im2, re2) / kTurn * (period / 2.0);
    const double lean = re1 * cos(kTurn * f * first) + im1 * sin(kTurn * f * first);
    if (lean < 0.0) first += period / 2.0;
  } else {
    first = atan2(im1, re1) / kTurn * period;
  }
  // The first beat: the one at (or within 50 ms before) the first onset that is not faint.
  const float strong = 0.3f * squares / total;
  double from = (double)m_.peaks[0].t;
  for (int i = 0; i < peakCount_; i++) {
    if (m_.peaks[i].w < strong) continue;
    from = (double)m_.peaks[i].t;
    break;
  }
  from -= 0.05;
  first -= floor((first - from) / period) * period;

  r.bpm = (float)(60.0 * f);
  r.firstBeat = (float)first;
  r.confidence = (float)((mag1 > mag2 ? mag1 : mag2) / (double)total);
  r.found = r.confidence >= 0.2f;
  return r;
}

}  // namespace dsp
