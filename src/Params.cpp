// Ported from teensy-ir-loader (src/Params.cpp) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#include "Params.h"

#include <Arduino.h>

#include "AmpLibrary.h"
#include "AudioChain.h"
#include <dsp/drums/DrumKit.h>
#include <dsp/drums/DrumMachine.h>
#include "DrumSongs.h"
#include "Settings.h"
#include "config.h"

using namespace ui;

static int songBars() {
  const dsp::DrumSong* s = currentDrumSong();
  return s ? s->barCount : 1;
}

int getParam(int id) {
  if (id >= 0 && id < kParamCount) return g_settings.v[id];
  switch (id) {
    case kDrumsPlay: return chain.drums().playing();
    case kDrumLoopFrom: return drumLoopFirst();
    case kDrumLoopTo: return drumLoopFirst() ? drumLoopLast() : songBars();
    default: return 0;
  }
}

void paramLimits(int id, int* lo, int* hi) {
  const ParamDef& d = paramDef(id);
  *lo = d.min;
  *hi = d.max;
  switch (id) {
    case kDelayTime: *hi = DELAY_MAX_MS; break;
    case kDrumStyle: *hi = dsp::drumStyleCount() - 1; break;
    case kDrumKit: *hi = dsp::drumKitCount() - 1; break;
    case kDrumSong: *hi = drumSongTotal(); break;
    case kDrumLoopFrom:
    case kDrumLoopTo: *hi = songBars(); break;
    default: break;
  }
}

void paramText(int id, char* buf, int n) {
  const int v = getParam(id);
  switch (id) {
    case kOutGain:
      if (v <= paramDef(id).min) {
        snprintf(buf, n, "silent");
        return;
      }
      break;
    case kDrumStyle: snprintf(buf, n, "%s", dsp::drumStyle(v).title); return;
    case kDrumKit: snprintf(buf, n, "%s", dsp::drumKit(v).name); return;
    case kDrumSong: snprintf(buf, n, "%s", v ? drumSongTitle(v) : "off"); return;
    case kDrumLoopFrom:
      if (!v) {
        snprintf(buf, n, "off");
        return;
      }
      break;
    default: break;
  }
  formatValue(paramDef(id), v, false, buf, n);
}

// What each setting does. Saved values are already in g_settings.
static void applyParam(int id, int v) {
  dsp::StudioDelay& d = chain.delay();
  dsp::Reverb80& r = chain.reverb();
  dsp::DrumMachine& dm = chain.drums();
  const int16_t* s = g_settings.v;
  switch (id) {
    case kDelayOn: chain.setDelayOn(v); break;
    case kDelayMix: chain.setDelayMix(v / 100.0f); break;
    case kDelayTime: d.setDelaySamples(v * (kSampleRate / 1000.0f)); break;
    case kDelayRepeats: d.setFeedback(v / 100.0f); break;
    case kDelayTone: d.setTone(v / 100.0f); break;
    case kDelayMod: d.setModDepth(v / 100.0f); break;
    case kDelayDuck: d.setDuck(v / 100.0f); break;
    case kDelayDrive: d.setDrive(v / 100.0f); break;
    case kDelayEra: d.setEra(v / 100.0f); break;
    case kReverbOn: chain.setReverbOn(v); break;
    case kReverbMix: chain.setReverbMix(v / 100.0f); break;
    case kReverbSize: r.setSize(v / 100.0f); break;
    case kReverbTone: r.setTone(v / 100.0f); break;
    case kReverbMode: r.setMode(v); break;
    case kReverbPreDelay: r.setPreDelayMs(v); break;
    case kReverbMod: r.setMod(v / 100.0f); break;
    case kReverbEra: r.setEra(v / 100.0f); break;
    case kReverbGate: r.setGateMs(v); break;
    // The master volume: the last thing before the DAC; its minimum is silence.
    case kOutGain: chain.setMasterDb(v <= paramDef(kOutGain).min ? -200.0f : v * 0.5f); break;
    case kPresetLevel: chain.setOutputGainDb(v * 0.5f); break;
    case kBypass: chain.setBypass(v); break;
    case kDrumStyle: dm.setStyle(v); break;
    case kDrumKit: dm.setKit(&dsp::drumKit(v)); break;
    case kDrumCrash: dm.setCrashEvery(v ? 1 << v : 0); break;  // 2, 4, 8, 16 bars
    case kDrumTempo: dm.setTempo(v); break;
    case kDrumVolume: dm.setVolume(v / 100.0f); break;
    case kDrumComp: dm.setBusComp(v / 100.0f); break;
    case kDrumCountIn: dm.setCountIn(v); break;
    case kTrainerOn:
    case kTrainerStep:
    case kTrainerEvery:
    case kTrainerTarget:
      dm.setTrainer(s[kTrainerOn], s[kTrainerStep], s[kTrainerEvery], s[kTrainerTarget]);
      break;
    case kBassOn: dm.setBassOn(v); break;
    case kBassVolume: dm.setBassVolume(v / 100.0f); break;
    case kBassKey: dm.setBassKey(v); break;
    case kBassTone: dm.setBassTone(v / 100.0f); break;
    case kAmpOn: chain.amp().setOn(v); break;
    case kAmpIn:
    case kAmpOut:
    case kAmpCal:
    case kAmpBass:
    case kAmpMid:
    case kAmpTreble:
    case kAmpPresence: ampApplyGains(); break;
    case kGateOn:
    case kGateThr:
      chain.gate().setThreshold(s[kGateOn] ? powf(10.0f, (float)s[kGateThr] / 20.0f) : 0.0f);
      break;
    case kGateRel: chain.gate().setRelease((float)v); break;
    case kGateType:
      chain.gate().setType(v ? dsp::NoiseGate<kBlock>::Type::kHard
                                 : dsp::NoiseGate<kBlock>::Type::kNam);
      break;
    case kDblOn: chain.doubler().setOn(v); break;
    case kDblTime:
    case kDblWidth: chain.doubler().set(s[kDblTime], s[kDblWidth] / 100.0f); break;
    case kDrumsPlay:
      if (v) dm.start();
      else dm.stop();
      break;
    case kDrumLoopFrom:
      if (!v) setDrumLoop(0, 0);
      else setDrumLoop(v, max(v, drumLoopFirst() ? drumLoopLast() : songBars()));
      break;
    case kDrumLoopTo: setDrumLoop(drumLoopFirst() ? drumLoopFirst() : 1, v); break;
    default: break;  // kDrumSong: selected in setParam
  }
}

// "DRUMS START: rock, 100 BPM, 2-bar count-in" / "DRUMS STOP".
static void printDrums(bool on) {
  if (!on) {
    Serial.println("DRUMS STOP");
    return;
  }
  const dsp::DrumSong* song = currentDrumSong();
  Serial.printf("DRUMS START: %s, %d BPM", song ? song->title : dsp::drumStyle(getParam(kDrumStyle)).title,
                getParam(kDrumTempo));
  if (getParam(kDrumCountIn)) Serial.printf(", %d-bar count-in", getParam(kDrumCountIn));
  Serial.println();
}

void setParamQuiet(int id, int v) {
  int lo, hi;
  paramLimits(id, &lo, &hi);
  v = constrain(v, lo, hi);
  if (id >= kParamCount || id == kDrumSong) return;
  g_settings.v[id] = v;
  scheduleSave();
  applyParam(id, v);
}

void setParam(int id, int v) {
  int lo, hi;
  paramLimits(id, &lo, &hi);
  v = constrain(v, lo, hi);
  if (id == kDrumSong && !selectDrumSong(v)) v = 0;  // prints its own message
  if (id < kParamCount) {
    g_settings.v[id] = v;
    scheduleSave();
  }
  applyParam(id, v);
  switch (id) {
    case kDrumSong:
    case kDrumLoopFrom:
    case kDrumLoopTo: break;  // their own messages
    case kDrumsPlay: printDrums(v); break;
    default: {
      char text[32];
      paramText(id, text, sizeof(text));
      Serial.printf("%s %s\n", paramDef(id).name, text);
    }
  }
}

void applyAllParams() {
  for (int id = 0; id < kParamCount; id++)
    if (id != kDrumSong) applyParam(id, g_settings.v[id]);
}
