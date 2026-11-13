// Ported from teensy-ir-loader (src/Settings.cpp) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#include "Settings.h"

#include <Arduino.h>
#include <EEPROM.h>

#include "Params.h"

Settings g_settings;

static const uint32_t kSettingsMagic = 0x4E464731;  // "NFG1": the paths, then values by ui::Param
static uint32_t g_saveAt = 0;  // millis() when a pending save is due (0 = none)

// The record up to 2026-11-13 ("IRLR", teensy-ir-loader's layout): the same with three more
// values after the output gain, the SGTL5000 codec's line-in, line-out and headphone levels.
// This hardware has no such settings; the record is read once to carry the others over.
static const uint32_t kCodecMagic = 0x49524C52;
constexpr int kCodecFirst = ui::kOutGain + 1, kCodecValues = 3;
struct CodecSettings {
  uint32_t magic;
  uint16_t count;
  uint16_t reserved;
  char irPath[IR_PATH_MAX];
  char ampPath[IR_PATH_MAX];
  char presetPath[IR_PATH_MAX];
  int16_t v[ui::kParamCount + kCodecValues];  // count of them were written
};

static void fromCodecRecord() {
  static CodecSettings old;  // static: not on the stack
  EEPROM.get(0, old);
  memset(&g_settings, 0, sizeof(g_settings));
  memcpy(g_settings.irPath, old.irPath, IR_PATH_MAX);
  memcpy(g_settings.ampPath, old.ampPath, IR_PATH_MAX);
  memcpy(g_settings.presetPath, old.presetPath, IR_PATH_MAX);
  const int had = old.count < ui::kParamCount + kCodecValues ? old.count : ui::kParamCount + kCodecValues;
  int n = 0;
  for (int i = 0; i < had; i++)
    if (i < kCodecFirst || i >= kCodecFirst + kCodecValues) g_settings.v[n++] = old.v[i];
  g_settings.count = n;
  g_settings.magic = kSettingsMagic;
  scheduleSave();
}

void loadSettings() {
  EEPROM.get(0, g_settings);
  if (g_settings.magic == kCodecMagic) fromCodecRecord();
  if (g_settings.magic != kSettingsMagic) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.magic = kSettingsMagic;
  }
  // Values the record doesn't have, or that are out of range, get their
  // defaults. (The song list isn't read yet: a song that has gone from the
  // card is dropped when it is selected at startup.)
  for (int i = 0; i < ui::kParamCount; i++) {
    int lo = ui::paramDef(i).min, hi = ui::paramDef(i).max;
    if (i != ui::kDrumSong) paramLimits(i, &lo, &hi);
    const int v = g_settings.v[i];
    if (i >= g_settings.count || v < lo || v > hi) g_settings.v[i] = ui::paramDef(i).def;
  }
  if (g_settings.count != ui::kParamCount) {
    g_settings.count = ui::kParamCount;
    scheduleSave();
  }
  g_settings.irPath[IR_PATH_MAX - 1] = 0;
  g_settings.ampPath[IR_PATH_MAX - 1] = 0;
  g_settings.presetPath[IR_PATH_MAX - 1] = 0;
}

void scheduleSave() { g_saveAt = millis() + 3000; if (!g_saveAt) g_saveAt = 1; }

void serviceSave() {
  if (g_saveAt && (int32_t)(millis() - g_saveAt) >= 0) {
    EEPROM.put(0, g_settings);
    g_saveAt = 0;
  }
}
