// Meters.cpp - see Meters.h.
#include "Meters.h"

#include "AudioChain.h"

static const int kSlots = 2 + AudioChain::kStageCount;  // input, output, the stages
static float g_kept[kMeterReaders][kSlots];

static float take(MeterReader who, int slot) {
  float v = slot == 0 ? chain.takeInputPeak() : (slot == 1 ? chain.takeOutputPeak() : chain.takeStagePeak(slot - 2));
  for (int r = 0; r < kMeterReaders; r++)
    if (r != who && v > g_kept[r][slot]) g_kept[r][slot] = v;
  if (g_kept[who][slot] > v) v = g_kept[who][slot];
  g_kept[who][slot] = 0.0f;
  return v;
}

float meterInputPeak(MeterReader who) { return take(who, 0); }
float meterOutputPeak(MeterReader who) { return take(who, 1); }
float meterStagePeak(MeterReader who, int stage) { return take(who, 2 + stage); }

// The limiter's lowest gain clears on reading too; here the lowest is what is kept.
static float g_keptGain[kMeterReaders] = {1.0f, 1.0f, 1.0f};
static_assert(kMeterReaders == 3, "one starting value for each reader");

float meterLimiterGain(MeterReader who) {
  float v = chain.takeLimiterGain();
  for (int r = 0; r < kMeterReaders; r++)
    if (r != who && v < g_keptGain[r]) g_keptGain[r] = v;
  if (g_keptGain[who] < v) v = g_keptGain[who];
  g_keptGain[who] = 1.0f;
  return v;
}
