// Ported from teensy-ir-loader (src/AmpLibrary.cpp) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#include "AmpLibrary.h"

#include <Arduino.h>
#include <SD.h>

#include "AudioChain.h"
#include "IRLibrary.h"
#include <dsp/nam/NamA2.h>
#include <dsp/nam/NamCache.h>
#include "Params.h"
#include "SdSource.h"
#include "Settings.h"
#include "config.h"

using namespace ui;

static const int kMaxAmps = AMP_MAX_FILES;
// The file list and the weights being handed to the audio side, in OCRAM:
// only the background touches them (the weights once, when swapped in).
DMAMEM static char g_files[kMaxAmps][IR_PATH_MAX];
DMAMEM static float g_staging[dsp::nama2::kWeightCount];
static int g_numFiles = 0;
static int g_current = -1;
static dsp::NamInfo g_info;

int ampCount() { return g_numFiles; }
int ampCurrent() { return g_current; }
const char* ampPath(int idx) { return g_files[idx]; }
const dsp::NamInfo& ampInfo() { return g_info; }

// Without a model on the card, the list holds one built-in entry: the
// NeuralAmpModelerCore example A2 model (test/fixtures/nam/A2.nam, MIT), kept
// in flash, so the amp block works with no SD card.
static const char kBuiltinPath[] = "(built-in)/A2 example.nam";
__asm__(".section .progmem.nammodel,\"a\"\n"
        ".balign 4\n"
        ".global g_builtinNamStart\ng_builtinNamStart:\n"
        ".incbin \"test/fixtures/nam/A2.nam\"\n"
        ".global g_builtinNamEnd\ng_builtinNamEnd:\n"
        ".previous\n");
extern "C" const uint8_t g_builtinNamStart[], g_builtinNamEnd[];

static bool isBuiltin(const char* path) { return !strcmp(path, kBuiltinPath); }

static void addBuiltinIfEmpty() {
  if (g_numFiles) return;
  strlcpy(g_files[0], kBuiltinPath, IR_PATH_MAX);
  g_numFiles = 1;
}

static bool isNamName(const char* n) {
  if (n[0] == '.') return false;  // hidden files, macOS "._" resource forks
  const size_t len = strlen(n);
  return len > 4 && strcasecmp(n + len - 4, ".nam") == 0;
}

static int cmpPath(const void* a, const void* b) { return strcasecmp((const char*)a, (const char*)b); }

void scanAmps() {
  g_numFiles = 0;
  g_current = -1;
  if (!irSdOk()) return addBuiltinIfEmpty();
  File d = SD.open("/AMP");
  if (!d || !d.isDirectory()) return addBuiltinIfEmpty();
  while (g_numFiles < kMaxAmps) {
    File e = d.openNextFile();
    if (!e) break;
    if (!e.isDirectory() && isNamName(e.name())) {
      const int n = snprintf(g_files[g_numFiles], IR_PATH_MAX, "/AMP/%s", e.name());
      if (n > 0 && n < IR_PATH_MAX) g_numFiles++;
      else Serial.printf("  skipped (path too long): %s\n", e.name());
    }
    e.close();
  }
  d.close();
  qsort(g_files, g_numFiles, IR_PATH_MAX, cmpPath);
  if (g_numFiles) Serial.printf("Found %d amp model(s) in /AMP.\n", g_numFiles);
  addBuiltinIfEmpty();
}

// s contains text, ignoring case.
static bool containsNoCase(const char* s, const char* text) {
  const size_t n = strlen(text);
  for (; *s; s++)
    if (!strncasecmp(s, text, n)) return true;
  return !n;
}

int findAmp(const char* text) {
  for (int i = 0; i < g_numFiles; i++)
    if (containsNoCase(strrchr(g_files[i], '/') + 1, text)) return i;
  return -1;
}

float ampInputFullScaleDbu() { return ADC_FULL_SCALE_DBU + INPUT_PAD_DB; }

float ampCalibrationDb() {
  if (g_current < 0 || !getParam(kAmpCal)) return NAN;
  const float ref = isnan(g_info.inputLevelDbu) ? AMP_UNCAL_REF_DBU : g_info.inputLevelDbu;
  return ampInputFullScaleDbu() - ref;
}

float ampLevelDb() { return g_current < 0 || isnan(g_info.loudness) ? 0.0f : AMP_LEVEL_DB - g_info.loudness; }

void ampApplyGains() {
  const float cal = ampCalibrationDb();
  const float in = 0.5f * (float)getParam(kAmpIn) + (isnan(cal) ? 0.0f : cal);
  const float out = ampLevelDb() + 0.5f * (float)getParam(kAmpOut);
  chain.amp().setGains(powf(10.0f, in / 20.0f), powf(10.0f, out / 20.0f));
  chain.amp().setTone(0.5f * getParam(kAmpBass), 0.5f * getParam(kAmpMid), 0.5f * getParam(kAmpTreble),
                          0.5f * getParam(kAmpPresence));
}

// ---- The weights cache (NamCache.h, PRESETS.md) --------------------------------------
// /AMP/.cache/<file>.bin holds what a full read of <file> got; the scan skips
// the folder (a directory, and a dot-name).

static const char kCacheDir[] = "/AMP/.cache";

// The file's modified time, packed (0 if the card has none).
static uint32_t stampOf(File& f) {
  DateTimeFields t;
  if (!f.getModifyTime(t)) return 0;
  return ((uint32_t)t.year << 24) | ((uint32_t)t.mon << 20) | ((uint32_t)t.mday << 15) | ((uint32_t)t.hour << 10) |
         ((uint32_t)t.min << 4) | (uint32_t)(t.sec >> 2);
}

// "/AMP/x.nam" -> "/AMP/.cache/x.nam.bin".
static bool cachePathOf(const char* path, char* out, int n) {
  const char* base = strrchr(path, '/');
  const int k = snprintf(out, n, "%s/%s.bin", kCacheDir, base ? base + 1 : path);
  return k > 0 && k < n;
}

static bool readCache(const char* path, uint32_t size, uint32_t stamp, dsp::NamInfo* info) {
  char cp[IR_PATH_MAX + 16];
  if (!cachePathOf(path, cp, sizeof(cp))) return false;
  File c = SD.open(cp, FILE_READ);
  if (!c) return false;
  SdSource src(c);
  const bool ok = dsp::readNamCache(src, size, stamp, g_staging, info);
  c.close();
  return ok;
}

void ampForgetCache(const char* path) {
  char cp[IR_PATH_MAX + 16];
  if (cachePathOf(path, cp, sizeof(cp)) && SD.exists(cp)) SD.remove(cp);
}

void ampMarkLoaded(const char* path) {
  g_current = -1;
  for (int i = 0; i < g_numFiles; i++)
    if (!strcmp(g_files[i], path)) g_current = i;
}

// Written through a temporary file, so a failed write leaves no half cache.
static void writeCache(const char* path, uint32_t size, uint32_t stamp, const dsp::NamInfo& info) {
  char cp[IR_PATH_MAX + 16], tmp[sizeof(kCacheDir) + 16];
  if (!cachePathOf(path, cp, sizeof(cp))) return;
  if (!SD.exists(kCacheDir) && !SD.mkdir(kCacheDir)) return;
  snprintf(tmp, sizeof(tmp), "%s/.saving.tmp", kCacheDir);
  SD.remove(tmp);
  File c = SD.open(tmp, FILE_WRITE);
  if (!c) return;
  const dsp::NamCacheHeader h = dsp::namCacheHeader(size, stamp, info, g_staging);
  c.write((const uint8_t*)&h, sizeof(h));
  c.write((const uint8_t*)&info, sizeof(info));
  c.write((const uint8_t*)g_staging, sizeof(g_staging));
  const bool ok = c.getWriteError() == 0;
  c.close();
  if (ok && (!SD.exists(cp) || SD.remove(cp)) && SD.rename(tmp, cp)) return;
  SD.remove(tmp);
  Serial.println("      (could not write its cache; the next load reads the .nam again)");
}

bool loadAmpIndex(int idx) {
  if (idx < 0 || idx >= g_numFiles) return false;
  dsp::AmpStage<kBlock>& amp = chain.amp();
  // The previous model is swapped in within a fade (20 ms).
  const uint32_t t = millis();
  while (!amp.weightsTaken()) {
    if (millis() - t > 500) {
      Serial.println("Busy, try again.");
      return false;
    }
    yield();
  }

  const char* path = g_files[idx];
  const uint32_t t0 = millis();
  dsp::NamInfo info;
  bool cached = false;
  if (isBuiltin(path)) {
    dsp::MemorySource src(g_builtinNamStart, (uint32_t)(g_builtinNamEnd - g_builtinNamStart));
    const char* err = "";
    if (!dsp::readNamA2Lite(src, g_staging, &info, &err)) {
      Serial.printf("Failed to load the built-in model: %s\n", err);
      return false;
    }
  } else {
    File f = SD.open(path, FILE_READ);
    if (!f) {
      Serial.printf("Cannot open %s\n", path);
      return false;
    }
    const uint32_t size = (uint32_t)f.size(), stamp = stampOf(f);
    // The cache first (~10 ms); else the .nam file itself (~0.9 s), and then
    // its cache is written.
    cached = readCache(path, size, stamp, &info);
    if (!cached) {
      SdSource src(f);
      const char* err = "";
      if (!dsp::readNamA2Lite(src, g_staging, &info, &err)) {
        f.close();
        Serial.printf("Failed to load %s: %s\n", path, err);
        return false;
      }
    }
    f.close();
    if (!cached) writeCache(path, size, stamp, info);
  }
  const uint32_t ms = millis() - t0;
  g_info = info;
  g_current = idx;
  amp.loadWeights(g_staging);
  ampApplyGains();
  strlcpy(g_settings.ampPath, path, IR_PATH_MAX);
  scheduleSave();

  Serial.printf("[amp %d/%d] %s\n      A2-Lite", idx + 1, g_numFiles, path);
  if (info.gearMake[0] || info.gearModel[0]) Serial.printf(", %s %s", info.gearMake, info.gearModel);
  if (info.gearType[0] || info.toneType[0]) Serial.printf(" (%s%s%s)", info.gearType, info.gearType[0] && info.toneType[0] ? ", " : "", info.toneType);
  Serial.printf(", level %+.1f dB", ampLevelDb());
  const float cal = ampCalibrationDb();
  if (!isnan(cal))
    Serial.printf(", input %.1f dBu%s -> %+.1f dB", isnan(info.inputLevelDbu) ? AMP_UNCAL_REF_DBU : info.inputLevelDbu,
                  isnan(info.inputLevelDbu) ? " (NAM default)" : "", cal);
  Serial.printf(", %lu ms%s\n", (unsigned long)ms, cached ? " (cache)" : "");
  if (fabsf(info.sampleRate - kSampleRate) > 1.0f)
    Serial.printf("      model is %.0f Hz, the firmware %.0f Hz: its tone is shifted\n", info.sampleRate,
                  kSampleRate);
  return true;
}

void loadStartupAmp() {
  for (int i = 0; i < g_numFiles; i++) {
    if (!strcasecmp(g_files[i], g_settings.ampPath)) {
      if (loadAmpIndex(i)) return;
      break;
    }
  }
  for (int i = 0; i < g_numFiles; i++)
    if (loadAmpIndex(i)) return;
}
