// MidiControl.cpp - see MidiControl.h.
#include "MidiControl.h"

#include <Arduino.h>

#include "Params.h"
#include "PresetLibrary.h"
#include "Tuner.h"

using namespace ui;

namespace {

// A controller and the setting it moves. 0..127 is spread over lo..hi (in the setting's own
// units: percent, ms, half-dB steps); lo == hi means the setting's whole range. A switch is
// on from 64 up.
struct CcMap {
  uint8_t cc;
  uint8_t param;
  int16_t lo, hi;
  bool isSwitch;
};

const CcMap kMap[] = {
    // Levels. Half-dB steps: -80 .. 0 is -40 .. 0 dB.
    {7, kOutGain, -80, 0, false},     // channel volume: output gain -40 .. 0 dB
    {16, kAmpIn, -24, 24, false},     // amp input (drive) -12 .. +12 dB
    {17, kAmpOut, -24, 24, false},    // amp output -12 .. +12 dB
    // Amp tone, each over its whole range.
    {70, kAmpBass, 0, 0, false},
    {71, kAmpMid, 0, 0, false},
    {72, kAmpTreble, 0, 0, false},
    {73, kAmpPresence, 0, 0, false},
    // Delay and reverb.
    {12, kDelayMix, 0, 0, false},
    {13, kDelayTime, 0, 0, false},
    {14, kDelayRepeats, 0, 0, false},
    {91, kReverbMix, 0, 0, false},    // the standard "reverb send" controller
    {92, kReverbSize, 0, 0, false},
    // Switches.
    {80, kAmpOn, 0, 1, true},
    {81, kGateOn, 0, 1, true},
    {83, kDelayOn, 0, 1, true},
    {84, kReverbOn, 0, 1, true},
    {85, kBypass, 0, 1, true},        // the cab IR's bypass
    {86, kDrumsPlay, 0, 1, true},
    {88, kModOn, 0, 1, true},         // 87 is the tuner, below
};
constexpr uint8_t kTunerCc = 87;      // the tuner (mutes the guitar while open)

void onControlChange(uint8_t cc, uint8_t value) {
  if (cc == kTunerCc) {
    tunerOpen(value >= 64);
    return;
  }
  for (const CcMap& m : kMap) {
    if (m.cc != cc) continue;
    int lo = m.lo, hi = m.hi;
    int limLo, limHi;
    paramLimits(m.param, &limLo, &limHi);
    if (lo == hi) lo = limLo, hi = limHi;
    int v = m.isSwitch ? (value >= 64 ? 1 : 0) : lo + ((hi - lo) * (int)value + 63) / 127;
    if (v < limLo) v = limLo;
    if (v > limHi) v = limHi;
    if (v == getParam(m.param)) return;  // a pedal sends many equal values
    // Quiet: a swept controller would flood the serial port. The drums' start and stop are
    // not a saved setting, so they go through setParam().
    if (m.param < kParamCount) setParamQuiet(m.param, v);
    else setParam(m.param, v);
    return;
  }
}

void onProgramChange(uint8_t program) {
  int idx = findPresetNumber(program + 1);
  if (idx < 0 && program < presetCount()) idx = program;
  if (idx < 0) {
    Serial.printf("MIDI program %d: no preset %02d\n", program, program + 1);
    return;
  }
  recallPreset(idx);
}

}  // namespace

void serviceMidi() {
  while (usbMIDI.read()) {
    switch (usbMIDI.getType()) {
      case usbMIDI.ControlChange: onControlChange(usbMIDI.getData1(), usbMIDI.getData2()); break;
      case usbMIDI.ProgramChange: onProgramChange(usbMIDI.getData1()); break;
      default: break;
    }
  }
}

void printMidiMap() {
  Serial.println("USB MIDI, any channel:");
  Serial.println("  Program Change n   preset n + 1");
  for (const CcMap& m : kMap) {
    const ParamDef& d = paramDef(m.param);
    int lo = m.lo, hi = m.hi;
    if (lo == hi) paramLimits(m.param, &lo, &hi);
    if (m.isSwitch) {
      Serial.printf("  CC %-3d             %s on/off (on from 64)\n", m.cc, d.name);
    } else {
      char a[16], b[16];
      formatValue(d, lo, true, a, sizeof(a));
      formatValue(d, hi, true, b, sizeof(b));
      Serial.printf("  CC %-3d             %s, %s .. %s\n", m.cc, d.name, a, b);
    }
  }
  Serial.printf("  CC %-3d             Tuner on/off (on from 64)\n", kTunerCc);
}
