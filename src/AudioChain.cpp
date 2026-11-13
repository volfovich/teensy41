// AudioChain.cpp - the chain's memory and its start-up. Ported from
// teensy-ir-loader's AudioGraph.cpp, without the Audio Library graph and the
// codec: the I2S driver (lib/i2s32) calls chain.process() from src/main.cpp.
#include "AudioChain.h"

#include <Arduino.h>

#include "Params.h"
#include "config.h"

AudioChain chain;

// Reverb lines live in the 512 KB OCRAM (DMAMEM): random access, but moving
// them to DTCM only saved 0.3 % CPU.
DMAMEM static float g_reverbMem[AudioChain::kReverbMemFloats];
// The two delay lines are too big for either RAM bank alone: one goes in
// DTCM, the other in OCRAM.
static const int kDelayLen = (int)(DELAY_MAX_MS * kSampleRate / 1000.0f) + 8;
static float g_delayA[kDelayLen];
DMAMEM static float g_delayB[kDelayLen];
// The bass's strings and its block buffers (13.6 KB).
DMAMEM static float g_bassMem[AudioChain::kBassBufLen];
// The doubler's line (8 KB) and the tuner's ring (16 KB).
DMAMEM static float g_doublerLine[AudioChain::kDoublerFloats];
DMAMEM static float g_tunerRing[AudioChain::kTunerRing];
// The mod's two lines (8 KB each).
DMAMEM static float g_modLineL[AudioChain::kModFloats], g_modLineR[AudioChain::kModFloats];
// The amp network's history (87 KB): read every block, so in DTCM (OCRAM
// measured 12 % slower).
static float g_ampHistory[AudioChain::kAmpHistoryFloats];

FLASHMEM void AudioChain::begin(float* reverbMem, float* delayA, float* delayB, int delayLen, float* bassBuf,
                                float* ampHistory, float* doublerLine, float* modLineL, float* modLineR,
                                float* tunerRing) {
  engine_.setFadeLength((int)(kSampleRate * IR_FADE_MS / 1000.0f));
  __disable_irq();
  reverb_.fx().init(reverbMem);
  delay_.fx().init(delayA, delayB, delayLen);
  doubler_.init(doublerLine);
  mod_.init(modLineL, modLineR);
  tunerRing_ = tunerRing;
  __enable_irq();
  drums_.setBassBuffer(bassBuf);
  amp_.begin(ampHistory);
}

FLASHMEM void audioBegin() {
  chain.begin(g_reverbMem, g_delayA, g_delayB, kDelayLen, g_bassMem, g_ampHistory, g_doublerLine, g_modLineL,
              g_modLineR, g_tunerRing);

  // The digital delay's fixed voicing (config.h); the rest is the saved settings.
  dsp::StudioDelay& d = chain.delay().studio();
  d.setRightRatio(DELAY80_RIGHT_RATIO);
  d.setCross(DELAY80_CROSS);
  d.setBassCut(DELAY80_BASS_CUT);
  d.setModRate(DELAY80_MOD_RATE_HZ);
  d.setDuckThresholdDb(DELAY80_DUCK_THRESHOLD_DB);
  d.setDuckReleaseMs(DELAY80_DUCK_RELEASE_MS);
  applyAllParams();
}
