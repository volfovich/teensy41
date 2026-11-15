// TimeStretch.cpp - see TimeStretch.h. All of it runs in the background. What runs for
// every window is left in RAM1 (no DSP_COLD): a window is 3 ms of work at first measure,
// several audio interrupts long, and each interrupt empties the instruction cache, which
// from flash is slow to fill again. For the same reason nothing here calls the maths
// library for each peak: the arc tangent, sine and cosine are worked out below.
#include <dsp/player/TimeStretch.h>

#include <math.h>
#include <string.h>

#include <dsp/Port.h>

#if defined(__arm__)
#include <arm_acle.h>
#endif

namespace dsp {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int kBits = 11;
static_assert((1 << kBits) == TimeStretch::kN, "kBits is the window's size");
// A window in which this much of the spectrum (the bins from 200 Hz up) rose fourfold is
// a hit, if that is half as much again as in the window before.
constexpr float kHitShare = 0.25f;
constexpr int kFirstHitBin = 9;
// A hit's windows: the one it is found in and as many more as it takes to pass through.
constexpr int kHitWindows = TimeStretch::kN / TimeStretch::kHop + 1;

// Where bin k is in the transform's buffer: its bits backwards.
inline int place(int k) {
#if defined(__arm__)
  return (int)(__rbit((uint32_t)k) >> (32 - kBits));
#else
  uint32_t x = (uint32_t)k;
  x = ((x >> 1) & 0x5555u) | ((x & 0x5555u) << 1);
  x = ((x >> 2) & 0x3333u) | ((x & 0x3333u) << 2);
  x = ((x >> 4) & 0x0F0Fu) | ((x & 0x0F0Fu) << 4);
  x = ((x >> 8) & 0x00FFu) | ((x & 0x00FFu) << 8);
  return (int)(x >> (16 - kBits));
#endif
}

// The whole number nearest to v (halves go up), also below zero, without the maths library.
inline int64_t nearest(double v) {
  v += 0.5;
  int64_t i = (int64_t)v;
  if ((double)i > v) i--;
  return i;
}

// The angle of (x, y) in turns (1 = a full circle), -0.5 .. 0.5, to about 2 millionths of
// a turn.
inline float turnsOf(float y, float x) {
  const float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
  const float hi = ax > ay ? ax : ay, lo = ax > ay ? ay : ax;
  if (hi <= 0.0f) return 0.0f;
  const float z = lo / hi, z2 = z * z;
  // atan(z) for 0 <= z <= 1, in radians.
  float a = z * (0.9998660f + z2 * (-0.3302995f + z2 * (0.1801410f + z2 * (-0.0851330f + z2 * 0.0208351f))));
  a *= 0.15915494f;                // to turns
  if (ay > ax) a = 0.25f - a;      // above the diagonal
  if (x < 0.0f) a = 0.5f - a;      // the left half
  return y < 0.0f ? -a : a;
}
}  // namespace

DSP_COLD TimeStretch::TimeStretch(const Memory& m) : m_(m) {
  re_ = m.work;
  im_ = re_ + kN;
  cos_ = im_ + kN;
  sin_ = cos_ + kN / 2;
  lastRe_ = sin_ + kN / 2;
  lastIm_ = lastRe_ + kBins;
  power_ = lastIm_ + kBins;
  accM_ = power_ + kBins;
  accS_ = accM_ + (kN - kHop);
  turn_ = accS_ + (kN - kHop);
  lastTurn_ = turn_ + 2 * kMaxPeaks;
  peak_ = m.index;
  lastPeak_ = peak_ + kMaxPeaks;
  lastOwner_ = lastPeak_ + kMaxPeaks;
  for (int i = 0; i < kN / 2; i++) {
    const float a = 2.0f * kPi * (float)i / (float)kN;
    cos_[i] = cosf(a);
    sin_[i] = sinf(a);
  }
  reset();
}

DSP_COLD void TimeStretch::setRate(float rate) {
  rate_ = rate < kMinRate ? kMinRate : (rate > kMaxRate ? kMaxRate : rate);
  if (windows_ == 0) centre_ = (double)(kN / 2 - 3 * kHop) * (double)rate_;  // see reset()
}

DSP_COLD void TimeStretch::reset() {
  base_ = fed_ = keepFrom_ = 0;
  // The output's first frame is the input's first: the first three windows reach back
  // before the stream (silence there), and what they alone give is not put out.
  centre_ = (double)(kN / 2 - 3 * kHop) * (double)rate_;
  lastStart_ = 0;
  windows_ = 0;
  lastPeaks_ = 0;
  lastRising_ = 0.0f;
  holdLeft_ = 0;
  owed_ = 0.0;
  outLeft_ = 0;
  outAt_ = 0.0;
  finished_ = false;
  memset(lastRe_, 0, sizeof(float) * 2 * kBins);
  memset(accM_, 0, sizeof(float) * 2 * (kN - kHop));
}

void TimeStretch::push(const int16_t* in, int frames) {
  if (frames > room()) frames = room();
  if (frames <= 0) return;
  // Make room at the end: what no later window can reach goes.
  if ((int)(fed_ - base_) + frames > kInFrames) {
    const uint64_t drop = keepFrom_ - base_;
    memmove(m_.in, m_.in + 2 * drop, sizeof(int16_t) * 2 * (size_t)(fed_ - base_ - drop));
    base_ += drop;
  }
  memcpy(m_.in + 2 * (fed_ - base_), in, sizeof(int16_t) * 2 * (size_t)frames);
  fed_ += (uint64_t)frames;
}

// The spectrum of re_ + i im_, in place; the bins come out in bit-reversed order
// (place()), which saves sorting them: inverse() takes them so. Two of the transform's
// eleven passes are done in one go over the data (the usual radix-2 steps, four points
// at a time); the last pass is left over.
void TimeStretch::forward() {
  float* const re = re_;
  float* const im = im_;
  for (int len = kN; len >= 4; len >>= 2) {
    const int q = len >> 2, step = kN / len;
    for (int start = 0; start < kN; start += len) {
      for (int j = 0; j < q; j++) {
        const float w1r = cos_[j * step], w1i = -sin_[j * step];
        const float w2r = cos_[2 * j * step], w2i = -sin_[2 * j * step];
        const int a0 = start + j, a1 = a0 + q, a2 = a1 + q, a3 = a2 + q;
        // The pass of length len: (a0, a2) with w1, (a1, a3) with w1 x -i.
        const float t0r = re[a0] + re[a2], t0i = im[a0] + im[a2];
        const float d0r = re[a0] - re[a2], d0i = im[a0] - im[a2];
        const float t1r = re[a1] + re[a3], t1i = im[a1] + im[a3];
        const float d1r = im[a1] - im[a3], d1i = re[a3] - re[a1];  // (x1 - x3) x -i
        const float t2r = d0r * w1r - d0i * w1i, t2i = d0r * w1i + d0i * w1r;
        const float t3r = d1r * w1r - d1i * w1i, t3i = d1r * w1i + d1i * w1r;
        // The pass of length len / 2: (a0, a1) and (a2, a3), both with w2.
        re[a0] = t0r + t1r;
        im[a0] = t0i + t1i;
        const float e0r = t0r - t1r, e0i = t0i - t1i;
        re[a1] = e0r * w2r - e0i * w2i;
        im[a1] = e0r * w2i + e0i * w2r;
        re[a2] = t2r + t3r;
        im[a2] = t2i + t3i;
        const float e1r = t2r - t3r, e1i = t2i - t3i;
        re[a3] = e1r * w2r - e1i * w2i;
        im[a3] = e1r * w2i + e1i * w2r;
      }
    }
  }
  for (int a = 0; a < kN; a += 2) {  // the pass of length 2
    const float tr = re[a] - re[a + 1], ti = im[a] - im[a + 1];
    re[a] += re[a + 1];
    im[a] += im[a + 1];
    re[a + 1] = tr;
    im[a + 1] = ti;
  }
}

// Back from the spectrum (bins in bit-reversed order) to the signal, kN times too large.
// The same passes backwards.
void TimeStretch::inverse() {
  float* const re = re_;
  float* const im = im_;
  for (int a = 0; a < kN; a += 2) {  // the pass of length 2
    const float tr = re[a + 1], ti = im[a + 1];
    re[a + 1] = re[a] - tr;
    im[a + 1] = im[a] - ti;
    re[a] += tr;
    im[a] += ti;
  }
  for (int len = 8; len <= kN; len <<= 2) {
    const int q = len >> 2, step = kN / len;
    for (int start = 0; start < kN; start += len) {
      for (int j = 0; j < q; j++) {
        const float ur = cos_[2 * j * step], ui = sin_[2 * j * step];
        const float vr = cos_[j * step], vi = sin_[j * step];
        const int a0 = start + j, a1 = a0 + q, a2 = a1 + q, a3 = a2 + q;
        // The pass of length len / 2: (a0, a1) and (a2, a3), both with u.
        const float m1r = re[a1] * ur - im[a1] * ui, m1i = re[a1] * ui + im[a1] * ur;
        const float m3r = re[a3] * ur - im[a3] * ui, m3i = re[a3] * ui + im[a3] * ur;
        const float p0r = re[a0] + m1r, p0i = im[a0] + m1i, p1r = re[a0] - m1r, p1i = im[a0] - m1i;
        const float p2r = re[a2] + m3r, p2i = im[a2] + m3i, p3r = re[a2] - m3r, p3i = im[a2] - m3i;
        // The pass of length len: (a0, a2) with v, (a1, a3) with v x i.
        const float n2r = p2r * vr - p2i * vi, n2i = p2r * vi + p2i * vr;
        const float n3r = -(p3r * vi + p3i * vr), n3i = p3r * vr - p3i * vi;
        re[a0] = p0r + n2r;
        im[a0] = p0i + n2i;
        re[a2] = p0r - n2r;
        im[a2] = p0i - n2i;
        re[a1] = p1r + n3r;
        im[a1] = p1i + n3i;
        re[a3] = p1r - n3r;
        im[a3] = p1i - n3i;
      }
    }
  }
}

// The spectrum the phases are read from, at bin k: 3/4 left + 1/4 right, which is M + S / 2
// with the sides' sum M[k] = (Z[k] + conj(Z[kN - k])) / 2 and difference
// S[k] = (Z[k] - conj(Z[kN - k])) / 2i. (Not the sum alone: sound that is on the two sides
// in opposite phase is not in it at all.)
void TimeStretch::mixAt(int k, float* r, float* i) const {
  const int a = place(k), b = place((kN - k) & (kN - 1));
  *r = 0.5f * (re_[a] + re_[b]) + 0.25f * (im_[a] + im_[b]);
  *i = 0.5f * (im_[a] - im_[b]) - 0.25f * (re_[a] - re_[b]);
}

// One window: kHop frames of output into m_.out. False if its input is not there yet.
bool TimeStretch::makeWindow() {
  const int64_t start = nearest(centre_) - kN / 2;
  if (!finished_ && (int64_t)fed_ < start + kN) return false;
  const int hopIn = windows_ ? (int)(start - lastStart_) : 0;  // frames since the last window

  // The window: the sides' sum as the real part, their difference as the imaginary part,
  // under a raised cosine (0.5 - 0.5 cos, from the transform's table).
  const bool held = start >= (int64_t)base_ && start + kN <= (int64_t)fed_;
  const int16_t* p = held ? m_.in + 2 * (start - (int64_t)base_) : nullptr;
  for (int i = 0; i < kN; i++) {
    const float c = i < kN / 2 ? cos_[i] : (i == kN / 2 ? -1.0f : cos_[kN - i]);
    const float w = 0.25f - 0.25f * c;  // with the half of (l + r) / 2
    const float l = held ? (float)p[2 * i] : (float)sampleAt(start + i, 0);
    const float r = held ? (float)p[2 * i + 1] : (float)sampleAt(start + i, 1);
    re_[i] = w * (l + r);
    im_[i] = w * (l - r);
  }
  forward();

  // The spectrum the phases are read from: three parts left and one part right, which is
  // the sum plus half the difference (see mixAt()). Its power, and how much of it rose
  // fourfold since the last window.
  int rose = 0;
  float loudest = 0.0f;
  for (int k = 0; k < kBins; k++) {
    float mr, mi;
    mixAt(k, &mr, &mi);
    power_[k] = mr * mr + mi * mi;
    if (power_[k] > loudest) loudest = power_[k];
    if (k >= kFirstHitBin && power_[k] > 4.0f * (lastRe_[k] * lastRe_[k] + lastIm_[k] * lastIm_[k]) + 1.0f) rose++;
  }
  const float rising = (float)rose / (float)(kBins - kFirstHitBin);
  // A hit: this window and the next ones, until the hit has passed through, are taken
  // kHop apart (as they are laid down) and keep the phases they have, so the hit comes out
  // exactly as it is. The time this takes from the tempo is made up after it. Not while
  // too much is owed already (hits in quick succession).
  // (The input's end, cut off under the window, also looks like a rise.)
  if (holdLeft_ == 0 && rising > kHitShare && rising > 1.5f * lastRising_ && owed_ < (double)kN && owed_ > -(double)kN &&
      start + kN <= (int64_t)fed_)
    holdLeft_ = kHitWindows;
  const bool asItIs = windows_ == 0 || hopIn <= 0 || lastPeaks_ == 0 || holdLeft_ > 0;
  lastRising_ = rising;

  // The peaks: a bin above its two neighbours on each side, and not more than 80 dB
  // under the loudest (what lies lower goes with its nearest peak).
  int peaks = 0;
  const float least = loudest * 1e-8f + 1.0f;
  for (int k = 2; k < kBins - 2 && peaks < kMaxPeaks; k++) {
    const float v = power_[k];
    if (v > least && v > power_[k - 1] && v > power_[k - 2] && v >= power_[k + 1] && v >= power_[k + 2]) {
      peak_[peaks++] = (uint16_t)k;
      k += 2;
    }
  }

  // Each peak's turn: from where its tone was laid down in the last window, on by its
  // frequency over kHop frames, to where it is in this window.
  for (int n = 0; n < peaks; n++) {
    float tr = 1.0f, ti = 0.0f;
    if (!asItIs) {
      const int k = peak_[n];
      float xr, xi;
      mixAt(k, &xr, &xi);
      // How far the phase moved since the last window, beyond what the bin's own
      // frequency gives (in turns): that is the tone's distance from the bin's frequency.
      const float ur = xr * lastRe_[k] + xi * lastIm_[k], ui = xi * lastRe_[k] - xr * lastIm_[k];
      float beyond = turnsOf(ui, ur) - (float)((k * hopIn) & (kN - 1)) * (1.0f / (float)kN);
      beyond -= (float)(int)(beyond + (beyond >= 0.0f ? 0.5f : -0.5f));
      // On by the tone's frequency over kHop frames: the bin's own share is k quarter
      // turns, the rest is what lay beyond. As a place in the transform's table of kN
      // steps to the turn, and between two of its steps.
      float at = beyond * ((float)kHop / (float)hopIn) * (float)kN;
      at -= (float)kN * (float)(int)(at * (1.0f / (float)kN));
      if (at < 0.0f) at += (float)kN;
      int step = (int)at;
      const float part = at - (float)step;
      step = (step + k * (kN / 4)) & (kN - 1);
      const int next = (step + 1) & (kN - 1);
      const float c0 = step < kN / 2 ? cos_[step] : -cos_[step - kN / 2], c1 = next < kN / 2 ? cos_[next] : -cos_[next - kN / 2];
      const float s0 = step < kN / 2 ? sin_[step] : -sin_[step - kN / 2], s1 = next < kN / 2 ? sin_[next] : -sin_[next - kN / 2];
      const float cr = c0 + part * (c1 - c0), ci = s0 + part * (s1 - s0);
      // Where the tone was laid down: the last window's peak this bin went with, as it
      // was turned then.
      const int owner = lastOwner_[k];
      const int ok = lastPeak_[owner];
      const float lr = lastRe_[ok], li = lastIm_[ok];
      const float yr = lr * lastTurn_[2 * owner] - li * lastTurn_[2 * owner + 1];
      const float yi = lr * lastTurn_[2 * owner + 1] + li * lastTurn_[2 * owner];
      // The turn: unit(y) x e^(i advance) x conj(unit(x)), with y x conj(x) made a unit
      // in one go.
      const float pr = yr * xr + yi * xi, pi = yi * xr - yr * xi;
      const float size = pr * pr + pi * pi;
      if (size > 1e-6f) {
        const float norm = 1.0f / __builtin_sqrtf(size);
        tr = (pr * cr - pi * ci) * norm;
        ti = (pr * ci + pi * cr) * norm;
      }
    }
    turn_[2 * n] = tr;
    turn_[2 * n + 1] = ti;
  }

  // This window's spectrum is kept for the next one, before it is turned.
  for (int k = 0; k < kBins; k++) mixAt(k, &lastRe_[k], &lastIm_[k]);
  // Every bin is turned as its peak is (the nearer of its two), both sides alike: bin k
  // by the turn, its mirror kN - k by the turn's conjugate. Bins 0 and kN / 2 stay.
  for (int n = 0, k = 1; n < peaks; n++) {
    const int upTo = n + 1 < peaks ? (peak_[n] + peak_[n + 1]) / 2 : kBins - 2;
    const float tr = turn_[2 * n], ti = turn_[2 * n + 1];
    for (; k <= upTo; k++) {
      lastOwner_[k] = (uint16_t)n;
      if (tr == 1.0f && ti == 0.0f) continue;
      const int a = place(k), b = place(kN - k);
      const float ar = re_[a], ai = im_[a], br = re_[b], bi = im_[b];
      re_[a] = ar * tr - ai * ti;
      im_[a] = ar * ti + ai * tr;
      re_[b] = br * tr + bi * ti;
      im_[b] = bi * tr - br * ti;
    }
  }
  if (peaks == 0) memset(lastOwner_, 0, sizeof(uint16_t) * kBins);
  lastOwner_[0] = 0;
  lastOwner_[kBins - 1] = (uint16_t)(peaks ? peaks - 1 : 0);
  float* const t = turn_;
  turn_ = lastTurn_;
  lastTurn_ = t;
  uint16_t* const q = peak_;
  peak_ = lastPeak_;
  lastPeak_ = q;
  lastPeaks_ = peaks;

  // Back to the signal, under the same window, onto what the last three windows laid
  // down. Raised cosines squared, four to a window's length, add up to 1.5.
  inverse();
  const float scale = 1.0f / (1.5f * (float)kN);
  for (int i = 0; i < kN; i++) {
    const float c = i < kN / 2 ? cos_[i] : (i == kN / 2 ? -1.0f : cos_[kN - i]);
    const float w = scale * (0.5f - 0.5f * c);
    re_[i] *= w;
    im_[i] *= w;
  }
  for (int i = 0; i < kHop; i++) {
    const float mid = accM_[i] + re_[i], side = accS_[i] + im_[i];
    for (int ch = 0; ch < 2; ch++) {
      const float v = ch ? mid - side : mid + side;
      const long r = (long)(v + (v >= 0.0f ? 0.5f : -0.5f));
      m_.out[2 * i + ch] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
  }
  for (int i = 0; i < kN - 2 * kHop; i++) {
    accM_[i] = accM_[i + kHop] + re_[i + kHop];
    accS_[i] = accS_[i + kHop] + im_[i + kHop];
  }
  for (int i = kN - 2 * kHop; i < kN - kHop; i++) {
    accM_[i] = re_[i + kHop];
    accS_[i] = im_[i + kHop];
  }

  // What is in out begins half a window before this window's middle (in the output),
  // which in the input is:
  outAt_ = centre_ - (double)(kN / 2) * (double)rate_;
  lastStart_ = start;
  // On to the next window: at the tempo's pace; through a hit at the track's own pace;
  // and after one, up to half as fast or half as slow again until the pace is caught up.
  double step = (double)kHop * (double)rate_;
  if (holdLeft_ > 1) {
    owed_ += (double)kHop - step;
    step = (double)kHop;
  } else if (owed_ != 0.0) {
    double pay = owed_;
    if (pay > 0.5 * step) pay = 0.5 * step;
    if (pay < -0.5 * step) pay = -0.5 * step;
    step -= pay;
    owed_ -= pay;
  }
  if (holdLeft_ > 0) holdLeft_--;
  centre_ += step;
  // The first three windows give nothing whole.
  outLeft_ = windows_ >= 3 ? kHop : 0;
  windows_++;
  // Input before the next window may go (push() drops it when it needs the room).
  int64_t keepFrom = nearest(centre_) - kN / 2;
  if (keepFrom > (int64_t)fed_) keepFrom = (int64_t)fed_;
  if (keepFrom > (int64_t)keepFrom_) keepFrom_ = (uint64_t)keepFrom;
  return true;
}

int TimeStretch::pull(int16_t* out, int maxFrames) {
  int n = 0;
  while (n < maxFrames) {
    if (outLeft_ == 0) {
      if (done() || !makeWindow()) break;
      continue;
    }
    const int count = outLeft_ < maxFrames - n ? outLeft_ : maxFrames - n;
    memcpy(out + 2 * n, m_.out + 2 * (kHop - outLeft_), sizeof(int16_t) * 2 * (size_t)count);
    n += count;
    outLeft_ -= count;
  }
  return n;
}

}  // namespace dsp
