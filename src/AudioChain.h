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
//   amped -> IR convolution -> IR bypass blend x output gain = core (mono)
//   core   -> doubler (mono in, stereo out, see dsp/fx/Doubler.h) -> cL, cR
//   cL, cR -> studio 80s delay (fed from cL, dry/wet)         -> dL, dR
//   dL, dR -> 80s reverb, hall or gated (stereo, dry/wet)      -> out L / R
//   + drum machine and bass (stereo, added to the outputs, never through
//     the effects)
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
#include <dsp/fx/Doubler.h>
#include <dsp/fx/FxStage.h>
#include <dsp/fx/NoiseGate.h>
#include <dsp/fx/Reverb80.h>
#include <dsp/fx/StudioDelay.h>
#include <dsp/nam/AmpStage.h>

#include "config.h"

class AudioChain {
 public:
  using Engine = dsp::PartitionedConvolver<kBlock, IR_MAX_LENGTH>;
  using DelayStage = dsp::FxStage<dsp::StudioDelay, kBlock, false>;
  using ReverbStage = dsp::FxStage<dsp::Reverb80, kBlock, true>;
  using Amp = dsp::AmpStage<kBlock>;
  static constexpr int kAmpHistoryFloats = Amp::kHistoryFloats;
  static constexpr int kReverbMemFloats = dsp::Reverb80::kMemFloats;
  static constexpr int kBassBufLen = dsp::DrumMachine::kBassBufLen;
  static constexpr int kDoublerFloats = dsp::Doubler::kLen;
  static constexpr int kTunerRing = 4096;  // floats at 24 kHz (170 ms)
  static constexpr float kTunerRate = kSampleRate / 2;

  AudioChain()
      : amp_(kSampleRate),
        gate_(kSampleRate),
        delay_(kSampleRate),
        reverb_(kSampleRate),
        doubler_(kSampleRate),
        drums_(kSampleRate) {
    tunerLp_.setLowpass(4000.0f, kSampleRate);
  }

  // Hands over the effects' memory before the audio runs: the reverb's lines
  // (kReverbMemFloats), the delay's two lines (delayLen floats each), the
  // bass's buffer (kBassBufLen), the amp's history (kAmpHistoryFloats), the
  // doubler's line (kDoublerFloats) and the tuner's ring (kTunerRing). Until
  // then the effects stay off.
  void begin(float* reverbMem, float* delayA, float* delayB, int delayLen, float* bassBuf, float* ampHistory,
             float* doublerLine, float* tunerRing);

  Engine& engine() { return engine_; }
  Amp& amp() { return amp_; }
  dsp::NoiseGate<kBlock>& gate() { return gate_; }
  dsp::StudioDelay& delay() { return delay_.fx(); }
  dsp::Reverb80& reverb() { return reverb_.fx(); }
  dsp::Doubler& doubler() { return doubler_; }
  dsp::DrumMachine& drums() { return drums_; }
  const dsp::DrumMachine& drums() const { return drums_; }

  // The preset's level: applied after the cab, before the doubler, delay and reverb.
  void setOutputGainDb(float db) { gainTarget_ = powf(10.0f, db / 20.0f); }
  // The master volume: applied last, to everything that goes to the DAC (the guitar with its
  // effects' tails, the drums and the bass). -200 or less is silence.
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
  enum Stage { kStageAmp, kStageGate, kStageCab, kStageDoubler, kStageDelay, kStageReverb, kStageCount };
  float takeStagePeak(int stage) { float p = stagePeak_[stage]; stagePeak_[stage] = 0.0f; return p; }
  // CPU load of this object, in percent of one core, and the shares of it
  // spent in the amp, the delay, the reverb and the drums.
  float cpuPercent() const { return cyclesToPercent(cyclesAvg_); }
  float cpuPercentMax() const { return cyclesToPercent(cyclesMax_); }
  float ampCpuPercent() const { return cyclesToPercent(amp_.cyclesAvg()); }
  float delayCpuPercent() const { return cyclesToPercent(delay_.cyclesAvg()); }
  float reverbCpuPercent() const { return cyclesToPercent(reverb_.cyclesAvg()); }
  float doublerCpuPercent() const { return cyclesToPercent(doublerCyclesAvg_); }
  float drumCpuPercent() const { return cyclesToPercent(drumCyclesAvg_); }
  void resetCpuMax() { cyclesMax_ = 0; }

  // One block: kBlock samples of guitar in, kBlock samples out for each side
  // (not yet clipped: the caller's conversion saturates; clips are counted
  // here). Runs in the audio interrupt.
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
    const float cabPk = peakOf(y);  // after the cab and the preset's level
    notePeak(kStageCab, cabPk);

    // The doubler, the delay, then the reverb. An idle effect passes its
    // input through, so with all idle the signal stays mono.
    const uint32_t tdb = ARM_DWT_CYCCNT;
    float cl[B], cr[B];
    const bool doubled = doubler_.process(y, cl, cr, B);
    const float* sl0 = doubled ? cl : y;
    const float* sr0 = doubled ? cr : y;
    const uint32_t t1 = ARM_DWT_CYCCNT;
    doublerCyclesAvg_ = doublerCyclesAvg_ + ((float)(t1 - tdb) - doublerCyclesAvg_) * 0.01f;
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
    const float dblPk = doubled ? fmaxf(peakOf(cl), peakOf(cr)) : cabPk;
    const float dlyPk = delayed ? fmaxf(peakOf(dl), peakOf(dr)) : dblPk;
    notePeak(kStageDoubler, dblPk);
    notePeak(kStageDelay, dlyPk);
    notePeak(kStageReverb, reverbed ? fmaxf(peakOf(ol), peakOf(orr)) : dlyPk);

    // Drums (stereo), added to the outputs below (nullptr while idle).
    const uint32_t t3 = ARM_DWT_CYCCNT;
    float drumL[B], drumR[B];
    const bool drums = drums_.process(drumL, drumR, B);
    const float* addL = drums ? drumL : nullptr;
    const float* addR = drums ? drumR : nullptr;
    drumCyclesAvg_ = drumCyclesAvg_ + ((float)(ARM_DWT_CYCCNT - t3) - drumCyclesAvg_) * 0.01f;

    float opk = 0.0f;  // this block's
    uint32_t clips = 0;
    const float m0 = masterCur_, m1 = ramp(m0, masterTarget_);  // 20 ms from silent to full
    masterCur_ = m1;
    if (!doubled && !delayed && !reverbed && !drums) {
      // Effects and drums idle: the dry signal goes to both outputs.
      toOutput(y, nullptr, m0, m1, outL, opk, clips);
      memcpy(outR, outL, sizeof(float) * B);
    } else {
      toOutput(fl, addL, m0, m1, outL, opk, clips);
      toOutput(fr, addR, m0, m1, outR, opk, clips);
    }
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

  // Adds add (if any), applies the master volume (m0 to m1 over the block) and clips to full
  // scale, counting the clipped samples.
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

  Amp amp_;
  dsp::NoiseGate<kBlock> gate_;
  Engine engine_;
  DelayStage delay_;
  ReverbStage reverb_;
  dsp::Doubler doubler_;
  dsp::DrumMachine drums_;
  volatile float drumCyclesAvg_ = 0.0f, doublerCyclesAvg_ = 0.0f;
  volatile bool tunerOn_ = false;
  float* tunerRing_ = nullptr;
  volatile float tunerTestStep_ = 0.0f;  // cycles per sample; 0 = the input
  float tunerTestPhase_ = 0.0f;
  volatile uint32_t tunerWritten_ = 0;  // samples since the tuner went on
  dsp::Biquad tunerLp_;
  float playCur_ = 1.0f;  // 0 while the tuner mutes
  volatile float gainTarget_ = 1.0f, gainCur_ = 1.0f;
  volatile float masterTarget_ = 0.5f;
  float masterCur_ = 0.0f;  // fades in at power-on
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
