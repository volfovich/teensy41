// Ported from teensy-ir-loader (src/Tuner.cpp) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#include "Tuner.h"

#include <Arduino.h>

#include "AudioChain.h"

static constexpr int kWindow = 2048;  // at 24 kHz: 85 ms, two periods of 24 Hz
static constexpr uint32_t kEveryMs = 100;
static constexpr uint32_t kHoldMs = 400;  // a reading stays this long after the note fades

static TunerReading g_reading;
static uint32_t g_lastAt = 0, g_validAt = 0;

void tunerOpen(bool on) {
  chain.setTuner(on);
  g_reading = TunerReading();
}

bool serviceTuner(uint32_t nowMs) {
  if (!chain.tunerOn() || nowMs - g_lastAt < kEveryMs) return false;
  g_lastAt = nowMs;
  const bool was = g_reading.valid;
  float window[kWindow];  // 8 KB of stack, only meanwhile (RAM2 is short)
  if (!chain.tunerSnapshot(window, kWindow)) return false;
  const dsp::Pitch p = dsp::PitchDetect::detect(window, kWindow, AudioChain::kTunerRate);
  if (p.hz > 0.0f) {
    const dsp::Note n = dsp::PitchDetect::note(p.hz);
    // The same note again: smooth the cents a little, so the needle settles.
    const bool same = was && n.index == g_reading.note.index && n.octave == g_reading.note.octave;
    const float before = g_reading.note.cents;
    g_reading.note = n;
    if (same) g_reading.note.cents = 0.5f * (before + n.cents);
    g_reading.hz = p.hz;
    g_reading.valid = true;
    g_validAt = nowMs;
    return true;
  }
  if (was && nowMs - g_validAt > kHoldMs) {
    g_reading.valid = false;
    return true;
  }
  return false;
}

const TunerReading& tunerReading() { return g_reading; }
