// Limiter.h - the Limiter block: a brickwall peak limiter for the stereo output, the last
// stage before the converter. The output never goes over the ceiling.
//
//   peak = max(|L|, |R|)                    both sides get the same gain: the image stays
//   need = peak > ceiling ? ceiling / peak : 1
//   env  = need at once when it is lower (the attack), held for 10 ms, then back up to 1
//          with the release time
//   out  = in x env                                              (without lookahead)
//
//   - Transparent: while the signal stays under the ceiling and the gain is back at 1,
//     process() does nothing and the samples pass bit for bit.
//   - The hold keeps the gain still between the half-waves of a low note (10 ms covers
//     50 Hz), and every further peak of about the same height renews it, so a steady
//     tone over the ceiling comes out as the same tone, quieter, instead of being
//     reshaped wave by wave.
//   - Without lookahead nothing is delayed. The gain drops at the very sample that would
//     go over, which is exact but sudden: clean on occasional peaks, a little less so
//     when driven hard.
//   - With lookahead the signal is delayed by kLook samples (1 ms) and the gain is the
//     lowest env of the next 1 ms, averaged over 1 ms: it glides down before the peak
//     arrives and is there in time. (Each averaged value is the lowest over a window
//     that contains the sample being played, so the result is never above what that
//     sample needs.) This adds 1 ms of latency while it is on: the one place in this
//     firmware where that is allowed, by the user's switch.
//   - Switching the lookahead (or the limiter with lookahead) on or off changes the delay,
//     so the output dips to silence and back within 5 ms instead of jumping.
//   - Sample peaks are limited, not the peaks between samples: keep the ceiling a little
//     under 0 dB (the default is -1 dB) for the converter's filter.
//
// Threading model: set() runs in the background, process() in the audio interrupt.
// set() fills the spare set of values, then switches to it.
#pragma once

#include <math.h>
#include <stdint.h>

#include <dsp/Port.h>

namespace dsp {

class Limiter {
 public:
  static constexpr int kLook = 48;  // the lookahead in samples (1 ms at 48 kHz)
  static constexpr float kHoldMs = 10.0f;
  // A peak within 0.3 dB of the one the gain is held for renews the hold. Without this the
  // gain would creep up between the peaks of a steady tone whenever a peak falls between
  // two samples, and be knocked down again: a ripple that is heard as roughness.
  static constexpr float kNear = 1.035f;

  explicit Limiter(float sampleRate)
      : fs_(sampleRate), holdLen_((int)(sampleRate * kHoldMs / 1000.0f)), fadeStep_(1.0f / (sampleRate * 0.002f)) {
    set(false, -1.0f, 100.0f, false);
    resetLook();
  }

  // Ceiling in dB (below 0), release in ms (the time to come 63 % of the way back).
  DSP_COLD void set(bool on, float ceilingDb, float releaseMs, bool lookahead) {
    Set& s = sets_[1 - cur_];
    s.on = on;
    s.look = on && lookahead;
    s.ceiling = powf(10.0f, (ceilingDb > 0.0f ? 0.0f : ceilingDb) / 20.0f);
    s.keep = expf(-1000.0f / (fs_ * (releaseMs < 1.0f ? 1.0f : releaseMs)));
    cur_ = 1 - cur_;
  }

  // The delay the limiter adds now, in samples (0 or kLook).
  int latencySamples() const { return look_ ? kLook : 0; }
  // True while process() has to run although the settings say off: the gain is still
  // releasing, or the delay is being switched.
  bool busy() const { return look_ || fade_ < 1.0f || fadeDown_ || cut_ > 0.0f; }
  bool on() const { return sets_[cur_].on; }
  // The lowest gain applied since the last call (1 = no limiting).
  float takeMinGain() {
    const float g = minGain_;
    minGain_ = 1.0f;
    return g;
  }

  // One block of n samples, in place. blockPeak is the largest |sample| of both sides in
  // it. Returns false when it did nothing: the samples are as they were.
  bool process(float* l, float* r, int n, float blockPeak) {
    const Set& s = sets_[cur_];
    if (s.look != look_ && !fadeDown_) fadeDown_ = true;  // the delay has to change
    const bool quiet = cut_ == 0.0f && hold_ == 0 && (!s.on || blockPeak <= s.ceiling);
    if (fade_ >= 1.0f && !fadeDown_) {
      if (quiet && !look_) return false;
      if (quiet) {  // lookahead with nothing to limit: the delay alone
        int at = pos_;
        for (int i = 0; i < n; i++) {
          const float dl = dlyL_[at], dr = dlyR_[at];
          dlyL_[at] = l[i];
          dlyR_[at] = r[i];
          l[i] = dl;
          r[i] = dr;
          if (++at == kLook) at = 0;
        }
        pos_ = at;
        idleLook_ = true;
        return true;
      }
    }
    if (idleLook_) {  // the gain history is all ones: start it afresh
      resetGains();
      idleLook_ = false;
    }
    float cut = cut_, fade = fade_, minGain = minGain_;
    int hold = hold_;
    for (int i = 0; i < n; i++) {
      const float xl = l[i], xr = r[i];
      const float al = fabsf(xl), ar = fabsf(xr), p = al > ar ? al : ar;
      const float need = s.on && p > s.ceiling ? s.ceiling / p : 1.0f;
      // The gain is kept as its distance below 1 (cut): close to 1 a float gain would
      // stop rising a hair short of it, and the limiter would never come to rest.
      const float needCut = 1.0f - need;
      if (needCut >= cut) {
        if (needCut > 0.0f) hold = holdLen_;
        cut = needCut;
      } else if (need < 1.0f && need <= (1.0f - cut) * kNear) {
        hold = holdLen_;  // a peak about as high as the one being held: keep holding
      } else if (hold > 0) {
        hold--;
      } else {
        cut *= s.keep;
        if (cut < 1e-5f) cut = 0.0f;
        if (cut < needCut) {
          cut = needCut;
          hold = holdLen_;
        }
      }
      const float env = 1.0f - cut;
      float g = env, yl = xl, yr = xr;
      if (look_) {
        g = lookGain(env);
        const int at = pos_;
        yl = dlyL_[at];
        yr = dlyR_[at];
        dlyL_[at] = xl;
        dlyR_[at] = xr;
        if (++pos_ == kLook) pos_ = 0;
      }
      if (g < minGain) minGain = g;
      if (fadeDown_ || fade < 1.0f) {
        if (fadeDown_) {
          fade -= fadeStep_;
          if (fade <= 0.0f) {  // silent: change the delay here
            fade = 0.0f;
            fadeDown_ = false;
            look_ = s.look;
            resetLook();
            wait_ = look_ ? kLook : 0;  // silent until the delay holds real samples
          }
        } else if (wait_ > 0) {
          wait_--;
        } else {
          fade += fadeStep_;
          if (fade > 1.0f) fade = 1.0f;
        }
        g *= fade;
      }
      l[i] = yl * g;
      r[i] = yr * g;
    }
    cut_ = cut;
    hold_ = hold;
    fade_ = fade;
    minGain_ = minGain;
    return true;
  }

 private:
  static constexpr int kRing = 64, kMask = kRing - 1;  // > kLook
  static constexpr int kWin = kLook + 1;               // the window: this sample and kLook to come

  struct Set {
    bool on = false, look = false;
    float ceiling = 1.0f, keep = 0.0f;  // keep: what is left of the cut after one sample of release
  };

  // The lookahead gain for the sample kLook back: the lowest env of the last kWin samples
  // (a queue that keeps only the values that can still become the lowest), averaged over
  // the last kWin of those.
  inline float lookGain(float env) {
    while (qCount_ > 0 && qVal_[(qHead_ + qCount_ - 1) & kMask] >= env) qCount_--;
    const int tail = (qHead_ + qCount_) & kMask;
    qVal_[tail] = env;
    qAt_[tail] = t_;
    qCount_++;
    if (t_ - qAt_[qHead_] >= (uint32_t)kWin) {
      qHead_ = (qHead_ + 1) & kMask;
      qCount_--;
    }
    const float low = qVal_[qHead_];
    const int at = (int)(t_ & kMask);
    const float gone = lows_[(at - kWin) & kMask];
    lows_[at] = low;
    sum_ += (double)low - (double)gone;  // in double: in float the sum drifts above its values
    below_ += (low < 1.0f) - (gone < 1.0f);
    t_++;
    return below_ == 0 ? 1.0f : (float)(sum_ * (1.0 / kWin));
  }
  void resetGains() {
    for (int i = 0; i < kRing; i++) lows_[i] = 1.0f;
    sum_ = (double)kWin;
    below_ = 0;
    qCount_ = 0;
    qHead_ = 0;
  }
  void resetLook() {
    for (int i = 0; i < kLook; i++) dlyL_[i] = dlyR_[i] = 0.0f;
    pos_ = 0;
    resetGains();
    idleLook_ = false;
  }

  const float fs_;
  const int holdLen_;
  const float fadeStep_;  // 2 ms down, 2 ms up
  Set sets_[2];
  volatile int cur_ = 0;
  volatile float minGain_ = 1.0f;
  // State (audio interrupt only).
  bool look_ = false, fadeDown_ = false, idleLook_ = false;
  float cut_ = 0.0f, fade_ = 1.0f;  // cut: 1 - the gain
  int hold_ = 0, wait_ = 0;
  // The delay: a ring of kLook samples; the place about to be written holds the sample
  // from kLook ago.
  float dlyL_[kLook], dlyR_[kLook];
  int pos_ = 0;
  float lows_[kRing];
  double sum_ = (double)kWin;
  int below_ = 0;
  float qVal_[kRing];
  uint32_t qAt_[kRing];
  int qHead_ = 0, qCount_ = 0;
  uint32_t t_ = 0;
};

}  // namespace dsp
