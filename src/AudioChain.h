// AudioChain.h - the whole signal chain, called once per block from the I2S
// driver's interrupt (lib/i2s32). Ported from teensy-ir-loader's
// AudioConvolverIR.h; there it was a Teensy Audio Library object with 16-bit
// input and output, here it is a plain class with float in and float out
// (the caller converts to and from the converters' 24 bits, dsp/Convert.h).
//
// One mono input (guitar), two outputs (left, right). The whole chain runs in
// 32-bit float inside process():
//
//   in -> amp (NAM A2-Lite, optional, see dsp/nam/AmpStage.h)
//      -> noise gate (listens to the input; gates the amp's output, or the
//         dry input with the amp off, see dsp/fx/NoiseGate.h)    = amped
//   amped -> IR convolution -> x (output gain + preset level), blended with
//            the IR's input by the bypass (bypassed: no IR and no gain)  = core (mono)
//   core   -> mod: chorus, flanger, ce2 or phase90 (stereo, see dsp/fx/Modulation.h) -> mL, mR
//   mL, mR -> delay: digital, tape or analog (fed from mL, dry/wet) -> dL, dR
//   dL, dR -> reverb: hall, gated, plate or spring (stereo, dry/wet) -> out L / R
//   + drum machine and bass (stereo, added to the outputs, never through
//     the effects)
//   + backing track (stereo, added beside the drums; see dsp/player/TrackOut.h. Its file is
//     read and decoded in the background, src/TrackLibrary.h; here a block is only copied
//     from its queue)
//   -> master volume (the last gain) -> limiter (optional, see dsp/fx/Limiter.h)
//   -> clip to full scale -> out L / R
//
// The tuner taps the input (decimated to 24 kHz, see dsp/fx/PitchDetect.h) and
// mutes the guitar, before the delay and reverb so their tails ring out.
//
// Adds smoothed output gain, click-free IR bypass, on/off for each effect
// (tails ring out when switched off; an idle effect passes its input through
// unchanged and costs no CPU, see dsp/fx/FxStage.h), peak meters, a clip
// counter and CPU measurement of its own process().
//
// The effects' and the drums' own setters are safe to call from the
// background (see their headers): reach them through delay(), reverb() and
// drums(), the amp's through amp() and the gate's through gate().
#pragma once

#include <Arduino.h>
#include <math.h>

#include <dsp/conv/PartitionedConvolver.h>
#include <dsp/drums/DrumMachine.h>
#include <dsp/fx/FxStage.h>
#include <dsp/fx/Limiter.h>
#include <dsp/fx/Modulation.h>
#include <dsp/fx/NoiseGate.h>
#include <dsp/fx/DelayBank.h>
#include <dsp/fx/ReverbBank.h>
#include <dsp/nam/AmpStage.h>
#include <dsp/player/TrackOut.h>

#include "config.h"

class AudioChain {
 public:
  using Engine = dsp::PartitionedConvolver<kBlock, IR_MAX_LENGTH>;
  using DelayStage = dsp::FxStage<dsp::DelayBank, kBlock, false>;
  using ReverbStage = dsp::FxStage<dsp::ReverbBank, kBlock, true>;
  using Amp = dsp::AmpStage<kBlock>;
  static constexpr int kAmpHistoryFloats = Amp::kHistoryFloats;
  static constexpr int kReverbMemFloats = dsp::ReverbBank::kMemFloats;
  static constexpr int kBassBufLen = dsp::DrumMachine::kBassBufLen;
  static constexpr int kModFloats = dsp::Modulation::kLen;  // each of its two lines
  static constexpr int kTunerRing = 4096;  // floats at 24 kHz (170 ms)
  static constexpr float kTunerRate = kSampleRate / 2;

  AudioChain()
      : amp_(kSampleRate),
        gate_(kSampleRate),
        delay_(kSampleRate),
        reverb_(kSampleRate),
        mod_(kSampleRate),
        limiter_(kSampleRate),
        drums_(kSampleRate),
        track_(kSampleRate) {
    tunerLp_.setLowpass(4000.0f, kSampleRate);
  }

  // Hands over the effects' memory before the audio runs: the reverb's lines
  // (kReverbMemFloats), the delay's two lines (delayLen floats each), the
  // bass's buffer (kBassBufLen), the amp's history (kAmpHistoryFloats), the
  // mod's two lines (kModFloats each) and
  // the tuner's ring (kTunerRing). Until then the effects stay off.
  void begin(float* reverbMem, float* delayA, float* delayB, int delayLen, float* bassBuf, float* ampHistory,
             float* modLineL, float* modLineR, float* tunerRing);

  Engine& engine() { return engine_; }
  Amp& amp() { return amp_; }
  dsp::NoiseGate<kBlock>& gate() { return gate_; }
  dsp::DelayBank& delay() { return delay_.fx(); }
  dsp::ReverbBank& reverb() { return reverb_.fx(); }
  dsp::Modulation& mod() { return mod_; }
  dsp::Limiter& limiter() { return limiter_; }
  // The lowest gain the limiter applied since the last call (1 = it did nothing).
  float takeLimiterGain() { return limiter_.takeMinGain(); }
  dsp::DrumMachine& drums() { return drums_; }
  // The backing track's output (its queue is handed over by TrackLibrary).
  dsp::TrackOut& track() { return track_; }

  // The output gain plus the preset's level: applied after the cab, before the mod,
  // delay and reverb (as in teensy-ir-loader; the drums, the bass and the backing track are not affected).
  void setOutputGainDb(float db) { gainTarget_ = powf(10.0f, db / 20.0f); }
  // The master volume: the last gain, on everything that goes to the DAC (the guitar with its
  // effects, the drums, the bass and the backing track). 0 dB leaves the samples exactly as they are; -200 or
  // less is silence.
  void setMasterDb(float db) { masterTarget_ = db <= -200.0f ? 0.0f : powf(10.0f, db / 20.0f); }
  void setBypass(bool on) { wetTarget_ = on ? 0.0f : 1.0f; }
  bool bypassed() const { return wetTarget_ < 0.5f; }

  // Effect on/off and mix (0 = dry only, 1 = wet only); "active" while on or
  // still ringing out.
  void setDelayOn(bool on) { delay_.setOn(on); }
  void setDelayMix(float m) { delay_.setMix(m); }
  bool delayActive() const { return delay_.running(); }
  void setReverbOn(bool on) { reverb_.setOn(on); }
  void setReverbMix(float m) { reverb_.setMix(m); }
  bool reverbActive() const { return reverb_.running(); }

  // The tuner: while on, the input is kept for tunerSnapshot() and the
  // guitar is muted.
  void setTuner(bool on) { tunerOn_ = on; }
  bool tunerOn() const { return tunerOn_; }
  // A test of the tuner without a guitar (serial "tuner test"): while hz is not 0 the tuner
  // hears a sine of that frequency at -20 dBFS instead of the input.
  void setTunerTestHz(float hz) { tunerTestStep_ = hz / kSampleRate; }
  // The newest n (<= kTunerRing / 2) samples at kTunerRate; false until
  // that many have come in since the tuner was switched on.
  bool tunerSnapshot(float* dst, int n) {
    for (int tries = 0; tries < 4; tries++) {
      const uint32_t w0 = tunerWritten_;
      if (w0 < (uint32_t)n) return false;
      for (int i = 0; i < n; i++) dst[i] = tunerRing_[(w0 - n + i) & (kTunerRing - 1)];
      if (tunerWritten_ - w0 <= (uint32_t)(kTunerRing - n)) return true;  // none overwritten meanwhile
    }
    return false;
  }

  // Meters: return the peak (linear, 1.0 = full scale) since the last call.
  float takeInputPeak() { float p = inPeak_; inPeak_ = 0.0f; return p; }
  float takeOutputPeak() { float p = outPeak_; outPeak_ = 0.0f; return p; }
  uint32_t clipCount() const { return clips_; }
  // The peak at a block's output since the last call, as takeInputPeak(). Above 1.0 is over
  // full scale: fine between blocks (the chain is float), but it would clip at the output.
  // In the order the app link sends them (PROTOCOL.md, METERS): new ones go at the end.
  enum Stage { kStageAmp, kStageGate, kStageCab, kStageDelay, kStageReverb, kStageMod, kStageLimiter, kStageCount };
  float takeStagePeak(int stage) { float p = stagePeak_[stage]; stagePeak_[stage] = 0.0f; return p; }
  // CPU load of this object, in percent of one core, and the shares of it
  // spent in the amp, the delay, the reverb and the drums.
  float cpuPercent() const { return cyclesToPercent(cyclesAvg_); }
  float cpuPercentMax() const { return cyclesToPercent(cyclesMax_); }
  float ampCpuPercent() const { return cyclesToPercent(amp_.cyclesAvg()); }
  float delayCpuPercent() const { return cyclesToPercent(delay_.cyclesAvg()); }
  float reverbCpuPercent() const { return cyclesToPercent(reverb_.cyclesAvg()); }
  float modCpuPercent() const { return cyclesToPercent(modCyclesAvg_); }
  float limiterCpuPercent() const { return cyclesToPercent(limCyclesAvg_); }
  float drumCpuPercent() const { return cyclesToPercent(drumCyclesAvg_); }
  float trackCpuPercent() const { return cyclesToPercent(trackCyclesAvg_); }
  void resetCpuMax() { cyclesMax_ = 0; }

  // One block: kBlock samples of guitar in, kBlock samples out for each side
  // (clipped to full scale and the clips counted, in toOutput()). Runs in the
  // audio interrupt.
  void process(const float* in, float* outL, float* outR) {
    const uint32_t t0 = ARM_DWT_CYCCNT;
    constexpr int B = kBlock;
    float x[B], a[B], y[B], dl[B], dr[B], ol[B], orr[B];

    float pk = 0.0f;  // this block's
    for (int i = 0; i < B; i++) {
      x[i] = in[i];
      pk = fmaxf(pk, fabsf(x[i]));
    }
    inPeak_ = fmaxf(inPeak_, pk);
    // The tuner's tap: low-passed, every second sample.
    if (tunerOn_ && tunerRing_) {
      uint32_t w = tunerWritten_;
      for (int i = 0; i < B; i++) {
        float t = x[i];
        if (tunerTestStep_ > 0.0f) {
          tunerTestPhase_ += tunerTestStep_;
          if (tunerTestPhase_ >= 1.0f) tunerTestPhase_ -= 1.0f;
          t = 0.1f * sinf(6.2831853f * tunerTestPhase_);
        }
        const float v = tunerLp_.process(t);
        if (i & 1) tunerRing_[w++ & (kTunerRing - 1)] = v;
      }
      tunerWritten_ = w;
    } else if (tunerWritten_) {
      tunerWritten_ = 0;
      tunerLp_.clear();
    }

    // The amp (idle: the input as it is).
    const uint32_t ta = ARM_DWT_CYCCNT;
    const bool amped = amp_.process(x, a);
    amp_.addCycles(ARM_DWT_CYCCNT - ta);
    float* src = amped ? a : x;
    const float ampPk = amped ? peakOf(a) : pk;
    notePeak(kStageAmp, ampPk);
    gate_.process(x, src);  // listens to the input (src may be x)
    notePeak(kStageGate, peakOf(src));

    engine_.process(src, y);

    // Per-sample linear ramps for gain and bypass (no zipper noise).
    const float g0 = gainCur_, g1 = gainTarget_;
    const float w0 = wetCur_;
    const float w1 = ramp(w0, wetTarget_);
    const float u0 = playCur_, u1 = ramp(u0, tunerOn_ ? 0.0f : 1.0f);  // the tuner's mute
    for (int i = 0; i < B; i++) {
      const float a = (float)(i + 1) / (float)B;
      const float g = g0 + (g1 - g0) * a;
      const float w = w0 + (w1 - w0) * a;
      const float u = u0 + (u1 - u0) * a;
      // Gain applies to the processed signal; bypass passes the IR's input.
      y[i] = u * (w * g * y[i] + (1.0f - w) * src[i]);
    }
    gainCur_ = g1;
    wetCur_ = w1;
    playCur_ = u1;
    const float cabPk = peakOf(y);  // after the cab, the output gain and the preset's level
    notePeak(kStageCab, cabPk);

    // The mod, the delay, then the reverb. An idle effect passes
    // its input through, so with all idle the signal stays mono.
    const float* sl0 = y;
    const float* sr0 = y;
    const uint32_t tm = ARM_DWT_CYCCNT;
    float ml[B], mr[B];
    const bool modded = mod_.process(sl0, sr0, ml, mr, B);
    if (modded) {
      sl0 = ml;
      sr0 = mr;
    }
    const uint32_t t1 = ARM_DWT_CYCCNT;
    modCyclesAvg_ = modCyclesAvg_ + ((float)(t1 - tm) - modCyclesAvg_) * 0.01f;
    const bool delayed = delay_.process(sl0, sr0, dl, dr);
    const uint32_t t2 = ARM_DWT_CYCCNT;
    delay_.addCycles(t2 - t1);
    const float* sl = delayed ? dl : sl0;
    const float* sr = delayed ? dr : sr0;
    const bool reverbed = reverb_.process(sl, sr, ol, orr);
    reverb_.addCycles(ARM_DWT_CYCCNT - t2);
    const float* fl = reverbed ? ol : sl;
    const float* fr = reverbed ? orr : sr;
    // An idle effect's output is its input: the level before it.
    const float modPk = modded ? fmaxf(peakOf(ml), peakOf(mr)) : cabPk;
    const float dlyPk = delayed ? fmaxf(peakOf(dl), peakOf(dr)) : modPk;
    notePeak(kStageMod, modPk);
    notePeak(kStageDelay, dlyPk);
    notePeak(kStageReverb, reverbed ? fmaxf(peakOf(ol), peakOf(orr)) : dlyPk);

    // Drums (stereo), added to the outputs below (nullptr while idle).
    const uint32_t t3 = ARM_DWT_CYCCNT;
    float drumL[B], drumR[B];
    const bool drums = drums_.process(drumL, drumR, B);
    const float* addL = drums ? drumL : nullptr;
    const float* addR = drums ? drumR : nullptr;
    const uint32_t t4 = ARM_DWT_CYCCNT;
    drumCyclesAvg_ = drumCyclesAvg_ + ((float)(t4 - t3) - drumCyclesAvg_) * 0.01f;

    // The backing track (stereo), added beside the drums (nothing while none plays).
    float trkL[B], trkR[B];
    const bool tracked = track_.process(trkL, trkR, B);
    if (tracked) {
      if (drums) {
        for (int i = 0; i < B; i++) {
          drumL[i] += trkL[i];
          drumR[i] += trkR[i];
        }
      } else {
        addL = trkL;
        addR = trkR;
      }
    }
    trackCyclesAvg_ = trackCyclesAvg_ + ((float)(ARM_DWT_CYCCNT - t4) - trackCyclesAvg_) * 0.01f;

    float opk = 0.0f;  // this block's
    uint32_t clips = 0;
    const float m0 = masterCur_, m1 = ramp(m0, masterTarget_);  // 20 ms between silent and full
    masterCur_ = m1;
    const bool mono = !modded && !delayed && !reverbed && !drums && !tracked;
    if (!limiter_.on() && !limiter_.busy()) {
      if (mono) {
        // Effects, drums and track idle: the dry signal goes to both outputs.
        toOutput(y, nullptr, m0, m1, outL, opk, clips);
        memcpy(outR, outL, sizeof(float) * B);
      } else {
        toOutput(fl, addL, m0, m1, outL, opk, clips);
        toOutput(fr, addR, m0, m1, outR, opk, clips);
      }
    } else {
      // With the limiter: the mix and the master volume first, unclipped; then the limiter
      // on both sides together; then the same clip as above (which it leaves nothing for).
      const uint32_t tl = ARM_DWT_CYCCNT;
      float pre = 0.0f;
      if (mono) {
        toMix(y, nullptr, m0, m1, outL, pre);
        memcpy(outR, outL, sizeof(float) * B);
      } else {
        toMix(fl, addL, m0, m1, outL, pre);
        toMix(fr, addR, m0, m1, outR, pre);
      }
      limiter_.process(outL, outR, B, pre);
      clipOutput(outL, opk, clips);
      clipOutput(outR, opk, clips);
      limCyclesAvg_ = limCyclesAvg_ + ((float)(ARM_DWT_CYCCNT - tl) - limCyclesAvg_) * 0.01f;
    }
    notePeak(kStageLimiter, opk);
    outPeak_ = fmaxf(outPeak_, opk);
    clips_ += clips;

    const uint32_t c = ARM_DWT_CYCCNT - t0;
    cyclesAvg_ = cyclesAvg_ + (c - cyclesAvg_) * 0.01f;
    if (c > cyclesMax_) cyclesMax_ = c;
  }

 private:
  static float cyclesToPercent(float cycles) {
    const float cyclesPerBlock = (float)F_CPU_ACTUAL * kBlock / kSampleRate;
    return 100.0f * cycles / cyclesPerBlock;
  }

  // Moves cur one block-length step toward target (0 -> 1 takes 20 ms).
  static float ramp(float cur, float target) {
    const float step = (float)kBlock / (kSampleRate * 0.02f);
    return target > cur ? fminf(cur + step, target) : fmaxf(cur - step, target);
  }

  static float peakOf(const float* v) {
    float p = 0.0f;
    for (int i = 0; i < kBlock; i++) p = fmaxf(p, fabsf(v[i]));
    return p;
  }
  void notePeak(int stage, float p) {
    if (p > stagePeak_[stage]) stagePeak_[stage] = p;
  }

  // Adds add (if any), applies the master volume (m0 to m1 over the block; at 1 the samples
  // are unchanged) and clips to full scale, counting the clipped samples.
  static void toOutput(const float* v, const float* add, float m0, float m1, float* out, float& peak, uint32_t& clips) {
    const float dm = (m1 - m0) / (float)kBlock;
    for (int i = 0; i < kBlock; i++) {
      float s = (add ? v[i] + add[i] : v[i]) * (m0 + dm * (float)(i + 1));
      const float a = fabsf(s);
      peak = fmaxf(peak, a);
      if (a > 0.9999999f) {
        clips++;
        s = s > 0 ? 0.9999999f : -0.9999999f;
      }
      out[i] = s;
    }
  }

  // toOutput() in two steps, for when the limiter comes between them: the sum and the
  // master volume (peak: the largest sample, before any clipping) ...
  static void toMix(const float* v, const float* add, float m0, float m1, float* out, float& peak) {
    const float dm = (m1 - m0) / (float)kBlock;
    for (int i = 0; i < kBlock; i++) {
      const float s = (add ? v[i] + add[i] : v[i]) * (m0 + dm * (float)(i + 1));
      peak = fmaxf(peak, fabsf(s));
      out[i] = s;
    }
  }
  // ... and the clip to full scale, counting the clipped samples.
  static void clipOutput(float* out, float& peak, uint32_t& clips) {
    for (int i = 0; i < kBlock; i++) {
      const float a = fabsf(out[i]);
      peak = fmaxf(peak, a);
      if (a > 0.9999999f) {
        clips++;
        out[i] = out[i] > 0 ? 0.9999999f : -0.9999999f;
      }
    }
  }

  Amp amp_;
  dsp::NoiseGate<kBlock> gate_;
  Engine engine_;
  DelayStage delay_;
  ReverbStage reverb_;
  dsp::Modulation mod_;
  dsp::Limiter limiter_;
  dsp::DrumMachine drums_;
  dsp::TrackOut track_;
  volatile float drumCyclesAvg_ = 0.0f, modCyclesAvg_ = 0.0f, limCyclesAvg_ = 0.0f, trackCyclesAvg_ = 0.0f;
  volatile bool tunerOn_ = false;
  float* tunerRing_ = nullptr;
  volatile float tunerTestStep_ = 0.0f;  // cycles per sample; 0 = the input
  float tunerTestPhase_ = 0.0f;
  volatile uint32_t tunerWritten_ = 0;  // samples since the tuner went on
  dsp::Biquad tunerLp_;
  float playCur_ = 1.0f;  // 0 while the tuner mutes
  volatile float gainTarget_ = 0.5f, gainCur_ = 0.5f;
  volatile float masterTarget_ = 1.0f;
  float masterCur_ = 1.0f;
  volatile float wetTarget_ = 1.0f, wetCur_ = 1.0f;
  volatile float inPeak_ = 0.0f, outPeak_ = 0.0f;
  volatile float stagePeak_[kStageCount] = {};
  volatile uint32_t clips_ = 0;
  volatile float cyclesAvg_ = 0.0f;
  volatile uint32_t cyclesMax_ = 0;
};

extern AudioChain chain;

// Hands the chain its memory and applies the saved settings (call
// loadSettings() first). Call before the I2S driver starts.
void audioBegin();
