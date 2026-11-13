// Teensy 4.1 guitar DSP platform: main firmware (see README.md and docs/).
//
//   guitar -> PCM1808 ADC (right) -> 30 Hz high-pass -> [NAM A2-Lite amp] -> noise gate
//          -> IR convolution (cab) -> output gain + preset level
//          -> doubler -> studio 80s delay -> 80s reverb -> + drums and bass
//          -> master volume -> PCM5102A DAC L/R
//          (32-bit float throughout, 24-bit converters, 48 kHz; docs/SIGNAL_CHAIN.md)
//
// The audio runs in the I2S driver's interrupt (lib/i2s32), one block at a time: the callback
// below converts the ADC's words to float, runs the chain (AudioChain.h) and converts back.
// The chain and everything around it are ported from the teensy-ir-loader project.
//
// Copy .wav impulse responses to the SD card (root or /IR folder) and NAM A2 amp models (.nam)
// to /AMP. Without a card, the amp block uses a built-in example model.
//
// Control: USB serial (type "?" for help) and USB MIDI (MidiControl.h).
//
//   AudioChain  the signal chain and its memory
//   Params      reading and changing any setting (the table: lib/params ParamDefs.cpp)
//   Settings    the settings saved in EEPROM
//   IRLibrary   SD card file list and IR loading
//   AmpLibrary  the NAM amp models in /AMP and loading them
//   DrumSongs   the drum songs, built-in and from the SD card
//   SerialUI    serial commands and level meter
//   AppLink     the desktop app's binary protocol on the same serial port (docs/APP.md)
//   Tuner       the tuner (pitch from the input while it is open)
#include <Arduino.h>
#include <I2s32.h>
#include <dsp/Blocks.h>
#include <dsp/Convert.h>

#include "AmpLibrary.h"
#include "AppLink.h"
#include "AudioChain.h"
#include "DrumSongs.h"
#include "IRLibrary.h"
#include "MidiControl.h"
#include "Params.h"
#include "SerialUI.h"
#include "Settings.h"
#include "config.h"

// Debug capture of the raw guitar input (serial command "cap"): 3 s into the PSRAM, then
// sent over USB as raw floats. For looking at noise and interference at the ADC.
static constexpr uint32_t kCaptureFrames = 3 * 48000;
EXTMEM static float g_capture[kCaptureFrames];
static volatile uint32_t g_captureLeft = 0;

void captureInput() {
  g_captureLeft = kCaptureFrames;
  while (g_captureLeft) delay(1);
  Serial.printf("CAPTURE %lu\n", (unsigned long)kCaptureFrames);
  Serial.write((const uint8_t*)g_capture, sizeof(g_capture));
  Serial.println("\nCAPTURE END");
}

// Test of the overload protection (serial command "overload"): for this many blocks the
// callback wastes 1.2 block periods, as a chain that is too slow would.
static volatile uint32_t g_overloadBlocks = 0;
void testOverload() { g_overloadBlocks = 400; }

// A test signal in place of the guitar (serial command "sig <Hz> <dBFS>", "sig 0" ends it): a
// sine at a known level, for checking the levels through the chain without a guitar.
static volatile float g_sigStep = 0.0f, g_sigAmp = 0.0f;
static float g_sigPhase = 0.0f;
void testSignal(float hz, float dbfs) {
  g_sigAmp = powf(10.0f, dbfs / 20.0f);
  g_sigStep = hz / kSampleRate;
}

// The overload protection (lib/i2s32, Config::faultAfterOverruns): when the chain cannot keep
// up for 16 blocks in a row the driver goes silent and stops calling it, which gives loop()
// the CPU back. Here the load is reduced, then the audio resumes after a second: first the
// drums and bass stop; if it trips again within 10 s the doubler, mod, delay and reverb go off too.
static void serviceFault() {
  static uint32_t faultAt = 0, lastFaultAt = 0;
  static bool handling = false;
  if (!i2s32::faulted()) return;
  const uint32_t now = millis();
  if (!handling) {
    handling = true;
    faultAt = now;
    g_overloadBlocks = 0;
    const bool again = lastFaultAt && now - lastFaultAt < 10000;
    lastFaultAt = now;
    Serial.println("\nAUDIO OVERLOAD: the chain could not keep up; output muted for 1 s.");
    if (getParam(ui::kDrumsPlay)) {
      setParam(ui::kDrumsPlay, 0);
    } else if (again) {
      Serial.println("Again: switching the doubler, mod, delay and reverb off.");
      setParam(ui::kDblOn, 0);
      setParam(ui::kModOn, 0);
      setParam(ui::kDelayOn, 0);
      setParam(ui::kReverbOn, 0);
    }
  } else if (now - faultAt >= 1000) {
    handling = false;
    i2s32::clearFault();
    Serial.println("Audio resumed.");
  }
}

// The input high-pass (INPUT_HIGHPASS_HZ in config.h): removes sub-audio thumps that reach
// the ADC through its supply before they can open the gate or drive the amp model.
static dsp::Biquad g_inputHp;

FASTRUN static void audioCallback(const int32_t* rx, int32_t* tx, size_t frames) {
  float inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock];
  dsp::int24InToFloat(rx, inL, inR, frames);
  float* const guitar = GUITAR_ON_RIGHT_CHANNEL ? inR : inL;
  if (g_captureLeft >= frames) {
    memcpy(g_capture + (kCaptureFrames - g_captureLeft), guitar, frames * sizeof(float));  // before the high-pass
    g_captureLeft = g_captureLeft - frames;
  } else {
    g_captureLeft = 0;
  }
  for (size_t i = 0; i < frames; i++) guitar[i] = g_inputHp.process(guitar[i]);
  if (g_sigStep > 0.0f) {
    for (size_t i = 0; i < frames; i++) {
      g_sigPhase += g_sigStep;
      if (g_sigPhase >= 1.0f) g_sigPhase -= 1.0f;
      guitar[i] = g_sigAmp * sinf(6.2831853f * g_sigPhase);
    }
  }
  chain.process(guitar, outL, outR);
  dsp::floatToInt24Out(outL, outR, tx, frames);
  if (g_overloadBlocks) {
    g_overloadBlocks = g_overloadBlocks - 1;
    const uint32_t t0 = ARM_DWT_CYCCNT, burn = i2s32::blockPeriodCycles() + i2s32::blockPeriodCycles() / 5;
    while (ARM_DWT_CYCCNT - t0 < burn) {}
  }
}

void setup() {
  loadSettings();
  // The chain gets its memory and the saved settings, then the audio starts: the passthrough
  // runs from here on, even before an IR or a model is loaded.
  audioBegin();
  g_inputHp.setHighpass(INPUT_HIGHPASS_HZ, kSampleRate);
  i2s32::Config cfg;
  cfg.callback = audioCallback;
  cfg.mutePin = DAC_MUTE_PIN;
  i2s32::begin(cfg);

  // No Serial.begin(): USB serial works without it on the Teensy. This line only shows if a
  // monitor is already open; every state can be printed later (?, s).
  Serial.printf("\nGuitar DSP - 48 kHz / 24-bit, block %d samples, max IR %d samples. Type ? for the commands.\n",
                kBlock, IR_MAX_LENGTH);

  scanCard();
  loadStartupIR();
  loadStartupAmp();
  // The drum song saved last time (an SD file may have gone: then none).
  if (getParam(ui::kDrumSong) && !selectDrumSong(getParam(ui::kDrumSong), true)) setParam(ui::kDrumSong, 0);
}

void loop() {
  serviceSerial();
  serviceMeter();
  serviceSave();
  serviceMidi();
  serviceAppLink();
  serviceFault();
}
