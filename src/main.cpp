// Teensy 4.1 guitar DSP platform: main firmware (see HLD.md).
//
//   guitar -> PCM1808 ADC (right) -> [NAM A2-Lite amp] -> noise gate -> IR convolution (cab)
//          -> doubler -> studio 80s delay -> 80s reverb -> PCM5102A DAC L/R
//          (32-bit float throughout, 24-bit converters, 48 kHz; drums and bass added after)
//
// The audio runs in the I2S driver's interrupt (lib/i2s32), one block at a time: the callback
// below converts the ADC's words to float, runs the chain (AudioChain.h) and converts back.
// The chain and everything around it are ported from the teensy-ir-loader project.
//
// Copy .wav impulse responses to the SD card (root or /IR folder) and NAM A2 amp models (.nam)
// to /AMP. Without a card, the amp block uses a built-in example model.
//
// Control: USB serial (type "?" for help).
//
//   AudioChain  the signal chain and its memory
//   Params      reading and changing any setting (the table: lib/params ParamDefs.cpp)
//   Settings    the settings saved in EEPROM
//   IRLibrary   SD card file list and IR loading
//   AmpLibrary  the NAM amp models in /AMP and loading them
//   DrumSongs   the drum songs, built-in and from the SD card
//   SerialUI    serial commands and level meter
//   Tuner       the tuner (pitch from the input while it is open)
#include <Arduino.h>
#include <I2s32.h>
#include <dsp/Convert.h>

#include "AmpLibrary.h"
#include "AudioChain.h"
#include "DrumSongs.h"
#include "IRLibrary.h"
#include "Params.h"
#include "SerialUI.h"
#include "Settings.h"
#include "config.h"

FASTRUN static void audioCallback(const int32_t* rx, int32_t* tx, size_t frames) {
  float inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock];
  dsp::int24InToFloat(rx, inL, inR, frames);
  chain.process(GUITAR_ON_RIGHT_CHANNEL ? inR : inL, outL, outR);
  dsp::floatToInt24Out(outL, outR, tx, frames);
}

void setup() {
  loadSettings();
  // The chain gets its memory and the saved settings, then the audio starts: the passthrough
  // runs from here on, even before an IR or a model is loaded.
  audioBegin();
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
  while (usbMIDI.read()) {}  // drain incoming MIDI; nothing is mapped to it yet
}
