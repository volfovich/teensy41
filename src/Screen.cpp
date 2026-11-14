// Screen.cpp - see Screen.h. Background only: this file, lib/ui and the display, touch and
// SPI libraries are linked into flash (tools/imxrt1062_t41_cold.ld).
#include "Screen.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <ILI9341_t3.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ui/Ui.h>

#include "AmpLibrary.h"
#include "AudioChain.h"
#include "IRLibrary.h"
#include "Meters.h"
#include "Params.h"
#include "PresetLibrary.h"
#include "Tuner.h"

using namespace ui_tft;

static ILI9341_t3 g_tft(kTftCs, kTftDc, kTftReset);
static XPT2046_Touchscreen g_touch(kTouchCs, kTouchIrq);

// One strip of pixels: a part of the screen is composed here, then sent as one rectangle.
static const int kStripPixels = kScreenW * 16;
DMAMEM static uint16_t g_strip[kStripPixels];

// ---- What is kept between power-ups ----
// Its own small record high in the EEPROM, away from the settings record at 0
// (Settings.cpp), so that record's layout stays as ported.
struct ScreenRecord {
  uint32_t magic;
  uint8_t mode;       // kAuto, kForcedOn, kForcedOff
  uint8_t calibrated;
  // The panel's raw readings at the screen points (20, 20) and (300, 220).
  int16_t rawX0, rawY0, rawX1, rawY1;
};
static const uint32_t kMagic = 0x54465431;  // "TFT1"
static const int kRecordAt = 3900;
enum { kAuto, kForcedOn, kForcedOff };
// Until calibrated: typical values of these boards (PJRC's ILI9341 touch example).
static ScreenRecord g_rec = {kMagic, kAuto, 0, 385, 453, 3572, 3677};

static bool g_present = false;  // a display answered at boot
static bool g_active = false;   // the screens are drawn and touch is read

// ---- The unit as the screens see it ----
static float toDb(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }

// A setting by its name in protocol/blocks.json: the serial key, or "@Name" for one
// without a key. -1 if the table has none (a block definition newer than the firmware).
static int findSetting(const char* ref) {
  for (int id = 0; id < ui::kValueCount; id++) {
    const ui::ParamDef& d = ui::paramDef(id);
    if (ref[0] == '@' ? !strcmp(d.name, ref + 1) : (d.key && !strcmp(d.key, ref))) return id;
  }
  return -1;
}

// Saved settings change quietly, as from a MIDI controller; the others (the drums' play
// switch, the song's loop) through setParam(), which they need.
static void changeSetting(int id, int v) {
  if (id < ui::kParamCount) setParamQuiet(id, v);
  else setParam(id, v);
}

struct UnitModel : UiModel {
  int blockParam[kBlockCount];
  int16_t settingId[kBlockCount][kMaxBlockSettings], onlyId[kBlockCount][kMaxBlockSettings];

  void begin() {
    for (int b = 0; b < kBlockCount; b++) {
      blockParam[b] = findSetting(kBlocks[b].onKey);
      for (int i = 0; i < kBlocks[b].settingCount; i++) {
        const BlockSetting& st = kBlocks[b].settings[i];
        settingId[b][i] = (int16_t)findSetting(st.ref);
        onlyId[b][i] = (int16_t)(st.onlyRef ? findSetting(st.onlyRef) : -1);
      }
    }
  }
  bool setting(int b, int i, SettingView* v) override {
    const int id = settingId[b][i];
    if (id < 0) return false;
    const BlockSetting& st = kBlocks[b].settings[i];
    if (st.onlyRef) {
      // Shown only while the type or mode it belongs to is chosen ("|hall|plate|").
      const ui::ParamDef& o = ui::paramDef(onlyId[b][i]);
      const int ov = onlyId[b][i] >= 0 ? getParam(onlyId[b][i]) : -1;
      if (ov < 0 || !o.choices || ov > o.max) return false;
      char want[24];
      snprintf(want, sizeof(want), "|%s|", o.choices[ov]);
      if (!strstr(st.onlyChoices, want)) return false;
    }
    const ui::ParamDef& d = ui::paramDef(id);
    // The short label, unless another setting of the block has the same one ("Volume" of
    // the drums and of the bass): then the full name.
    const char* label = d.label;
    for (int j = 0; j < kBlocks[b].settingCount; j++)
      if (j != i && settingId[b][j] >= 0 && !strcmp(ui::paramDef(settingId[b][j]).label, d.label)) label = d.name;
    strlcpy(v->label, label, sizeof(v->label));
    paramText(id, v->text, sizeof(v->text));
    int lo, hi;
    paramLimits(id, &lo, &hi);
    const int val = getParam(id);
    v->toggle = d.unit == ui::Unit::kOnOff;
    v->on = val != 0;
    v->bar = !v->toggle && !d.choices && d.unit != ui::Unit::kName && hi > lo;
    v->fraction = hi > lo ? (float)(val - lo) / (float)(hi - lo) : 0.0f;
    return true;
  }
  void settingStep(int b, int i, int steps) override {
    const int id = settingId[b][i];
    if (id < 0) return;
    const ui::ParamDef& d = ui::paramDef(id);
    int lo, hi;
    paramLimits(id, &lo, &hi);
    int v = d.unit == ui::Unit::kOnOff ? (steps > 0) : getParam(id) + steps * (d.step ? d.step : 1);
    v = v < lo ? lo : (v > hi ? hi : v);
    if (v != getParam(id)) changeSetting(id, v);
  }
  int presetCount() override { return ::presetCount(); }
  int presetCurrent() override { return ::presetCurrent(); }
  int presetSaved() override { return ::presetSaved(); }
  const char* presetName(int idx) override { return ::presetName(idx); }
  int presetNumber(int idx) override { return ::presetNumber(idx); }
  bool presetEdited() override { return ::presetEdited(); }
  bool recall(int idx) override { return recallPreset(idx); }
  bool preview(int idx) override { return previewPreset(idx); }
  void cancelPreview() override { ::cancelPreview(); }
  void keepPreview() override { ::keepPreview(); }
  bool save() override { return savePreset(); }
  bool blockOn(int b) override { return blockParam[b] >= 0 && (getParam(blockParam[b]) != 0) != kBlocks[b].onInverted; }
  void toggleBlock(int b) override {
    const int id = blockParam[b];
    if (id >= 0) changeSetting(id, !getParam(id));
  }
  int fileCount(FileKind k) override { return k == kFileModel ? ampCount() : irCount(); }
  int fileCurrent(FileKind k) override { return k == kFileModel ? ampCurrent() : irCurrent(); }
  // The file's name as it reads best on a small screen: no folder, no extension, spaces
  // for underscores.
  const char* fileName(FileKind k, int idx) override {
    static char buf[64];
    const char* path = k == kFileModel ? ampPath(idx) : irPath(idx);
    const char* slash = strrchr(path, '/');
    strlcpy(buf, slash ? slash + 1 : path, sizeof(buf));
    char* dot = strrchr(buf, '.');
    if (dot && dot != buf && strlen(dot) <= 5) *dot = 0;
    for (char* c = buf; *c; c++)
      if (*c == '_') *c = ' ';
    return buf;
  }
  bool fileLoad(FileKind k, int idx) override { return k == kFileModel ? loadAmpIndex(idx) : loadIRIndex(idx); }
  float inputDb() override { return toDb(meterInputPeak(kMeterScreen)); }
  float outputDb() override { return toDb(meterOutputPeak(kMeterScreen)); }
  int cpuPercent() override { return (int)(chain.cpuPercent() + 0.5f); }
  void tunerOpen(bool on) override { ::tunerOpen(on); }
  TunerView tuner() override {
    const TunerReading& r = tunerReading();
    TunerView v;
    v.valid = chain.tunerOn() && r.valid;
    v.note = r.note.index;
    v.octave = r.note.octave;
    v.cents = r.note.cents;
    v.hz = r.hz;
    return v;
  }
};
static UnitModel g_model;
static Ui g_ui(g_model);

// ---- Touch ----
// Raw readings to screen points through the two calibration points.
static void toScreen(int rawX, int rawY, int* x, int* y) {
  const int dx = g_rec.rawX1 - g_rec.rawX0, dy = g_rec.rawY1 - g_rec.rawY0;
  *x = 20 + (dx ? (rawX - g_rec.rawX0) * 280 / dx : 0);
  *y = 20 + (dy ? (rawY - g_rec.rawY0) * 200 / dy : 0);
  *x = *x < 0 ? 0 : (*x > kScreenW - 1 ? kScreenW - 1 : *x);
  *y = *y < 0 ? 0 : (*y > kScreenH - 1 ? kScreenH - 1 : *y);
}

static uint32_t g_touchMs = 0;
// A tap asked for over the shell ('tft tap'): down until g_tapUntil.
static uint32_t g_tapUntil = 0;
static int g_tapX = 0, g_tapY = 0;

// Reads the panel at most every 10 ms. True with the raw point while it is touched.
static bool readTouch(uint32_t now, int* rawX, int* rawY, bool* fresh) {
  static bool down = false;
  static int lastX = 0, lastY = 0;
  *fresh = false;
  if (now - g_touchMs >= 10) {
    g_touchMs = now;
    *fresh = true;
    down = g_touch.touched();
    if (down) {
      const TS_Point p = g_touch.getPoint();
      lastX = p.x;
      lastY = p.y;
    }
  }
  *rawX = lastX;
  *rawY = lastY;
  return down;
}

// ---- Calibration: two crosses, each touched once ----
static int g_calStep = 0;  // 0 = not calibrating, 1 and 2 = waiting for a cross
static uint32_t g_calHeldMs = 0;
static bool g_calReleased = true;
static int32_t g_calSumX = 0, g_calSumY = 0, g_calN = 0;
static int16_t g_calRaw[2][2];

static void drawCross(int step) {
  g_tft.fillScreen(ILI9341_BLACK);
  g_tft.setTextColor(ILI9341_WHITE);
  g_tft.setTextSize(2);
  g_tft.setCursor(64, 100);
  g_tft.print("Touch the cross");
  g_tft.setCursor(64, 124);
  g_tft.print(step == 1 ? "(1 of 2)" : "(2 of 2)");
  const int x = step == 1 ? 20 : 300, y = step == 1 ? 20 : 220;
  g_tft.drawFastHLine(x - 12, y, 25, ILI9341_YELLOW);
  g_tft.drawFastVLine(x, y - 12, 25, ILI9341_YELLOW);
}

static void saveRecord() { EEPROM.put(kRecordAt, g_rec); }

static void startCalibration() {
  g_calStep = 1;
  g_calReleased = false;  // a finger already down does not count
  g_calN = 0;
  drawCross(1);
}

static void serviceCalibration(uint32_t now) {
  int rx, ry;
  bool fresh;
  const bool down = readTouch(now, &rx, &ry, &fresh);
  if (!fresh) return;
  if (!down) {
    g_calReleased = true;
    g_calN = 0;
    return;
  }
  if (!g_calReleased) return;
  if (!g_calN) g_calHeldMs = now, g_calSumX = g_calSumY = 0;
  g_calSumX += rx;
  g_calSumY += ry;
  g_calN++;
  if (now - g_calHeldMs < 300) return;  // held for 0.3 s: the average of the readings
  g_calRaw[g_calStep - 1][0] = (int16_t)(g_calSumX / g_calN);
  g_calRaw[g_calStep - 1][1] = (int16_t)(g_calSumY / g_calN);
  g_calReleased = false;
  g_calN = 0;
  if (g_calStep == 1) {
    g_calStep = 2;
    drawCross(2);
    return;
  }
  g_calStep = 0;
  // The two crosses are far apart in both directions; readings that are not are a slip.
  if (abs(g_calRaw[1][0] - g_calRaw[0][0]) > 1000 && abs(g_calRaw[1][1] - g_calRaw[0][1]) > 1000) {
    g_rec.rawX0 = g_calRaw[0][0];
    g_rec.rawY0 = g_calRaw[0][1];
    g_rec.rawX1 = g_calRaw[1][0];
    g_rec.rawY1 = g_calRaw[1][1];
    g_rec.calibrated = 1;
    saveRecord();
    Serial.printf("Touch calibrated: raw (%d, %d) and (%d, %d).\n", g_rec.rawX0, g_rec.rawY0, g_rec.rawX1, g_rec.rawY1);
  } else {
    Serial.println("Touch calibration failed (the two points read too close): type 'tft cal' to try again.");
  }
  g_ui.invalidate();
}

// ---- The drawing bench (debug): what drawing costs the audio ----
enum Bench { kOff, kMeters, kFull };
static Bench g_bench = kOff;
static uint32_t g_nextMs = 0, g_frames = 0, g_drawUs = 0, g_worstUs = 0, g_reportMs = 0, g_rawUntil = 0;
static int g_fullY = 0;

// A stand-in for composing a part: every pixel is computed and written.
static void compose(int w, int h, uint32_t seed) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) g_strip[y * w + x] = (uint16_t)((x * 2654435761u + y * 40503u + seed) >> 12);
}

static void timed(uint32_t t0) {
  const uint32_t us = micros() - t0;
  g_drawUs += us;
  if (us > g_worstUs) g_worstUs = us;
  g_frames++;
}

static void serviceBench(uint32_t now) {
  if (g_bench == kMeters) {
    // Two bars 25 times a second, and once a second three strips (as a preset change).
    if (now < g_nextMs) return;
    g_nextMs = now + 40;
    const uint32_t t0 = micros();
    for (int m = 0; m < 2; m++) {
      compose(150, 10, now + m);
      g_tft.writeRect(10 + 160 * m, 220, 150, 10, g_strip);
    }
    if (g_frames % 25 == 0)
      for (int s = 0; s < 3; s++) {
        compose(kScreenW, 16, now + s);
        g_tft.writeRect(0, 40 + s * 16, kScreenW, 16, g_strip);
      }
    timed(t0);
  } else {
    // The worst the screen can do: repainting everything all the time, one strip a pass.
    const uint32_t t0 = micros();
    compose(kScreenW, 16, now);
    g_tft.writeRect(0, g_fullY, kScreenW, 16, g_strip);
    g_fullY = (g_fullY + 16) % kScreenH;
    timed(t0);
  }
  if (now - g_reportMs >= 5000) {
    Serial.printf("tft bench: %lu draws in 5 s, %.2f ms average, %.2f ms worst\n", (unsigned long)g_frames,
                  g_frames ? g_drawUs / 1000.0f / g_frames : 0.0f, g_worstUs / 1000.0f);
    g_reportMs = now;
    g_frames = g_drawUs = g_worstUs = 0;
  }
}

// ---- Service ----
static void activate(bool on) {
  if (on && !g_active) g_ui.invalidate();
  if (!on && g_active && g_ui.screen() == Ui::kTuner && chain.tunerOn()) tunerOpen(false);  // or the guitar stays muted
  g_active = on;
}

void screenBegin() {
  ScreenRecord r;
  EEPROM.get(kRecordAt, r);
  if (r.magic == kMagic) g_rec = r;
  g_model.begin();
  g_tft.begin();
  g_tft.setRotation(1);  // landscape, 320 x 240
  g_touch.begin();
  g_touch.setRotation(1);
  // With no display attached MISO and the touch interrupt line are open. Weak pulls give
  // them a level (a display overrides both): MISO then reads zeros, and the interrupt line
  // cannot flutter.
  CORE_PIN12_PADCONFIG |= IOMUXC_PAD_PKE | IOMUXC_PAD_PUE | IOMUXC_PAD_PUS(0);
  pinMode(kTouchIrq, INPUT_PULLUP);
  // A display answers the power-mode read with some bits set, the same each time.
  const uint8_t mode = g_tft.readcommand8(ILI9341_RDMODE);
  g_present = mode != 0x00 && mode != 0xFF && g_tft.readcommand8(ILI9341_RDMODE) == mode;
  if (!g_present) detachInterrupt(digitalPinToInterrupt(kTouchIrq));
  activate(g_rec.mode == kForcedOn || (g_rec.mode == kAuto && g_present));
  if (g_active && g_present && !g_rec.calibrated) startCalibration();
}

void serviceScreen() {
  const uint32_t now = millis();
  if (g_rawUntil) {  // 'tft touch'
    int rx, ry;
    bool fresh;
    if (now > g_rawUntil) {
      g_rawUntil = 0;
      Serial.println("Touch readout ended.");
      g_ui.invalidate();
    } else if (readTouch(now, &rx, &ry, &fresh) && fresh && now >= g_nextMs) {
      int x, y;
      toScreen(rx, ry, &x, &y);
      Serial.printf("touch raw %d, %d -> screen %d, %d\n", rx, ry, x, y);
      g_nextMs = now + 100;
    }
    return;
  }
  if (g_bench != kOff) return serviceBench(now);
  if (g_calStep) return serviceCalibration(now);
  if (!g_active) return;

  if (g_tapUntil) {
    const bool down = now < g_tapUntil;
    g_ui.touch(down, g_tapX, g_tapY, now);
    if (!down && now - g_tapUntil > 100) g_tapUntil = 0;
  } else if (g_present) {
    int rx, ry, x, y;
    bool fresh;
    const bool down = readTouch(now, &rx, &ry, &fresh);
    if (fresh) {
      toScreen(rx, ry, &x, &y);
      g_ui.touch(down, x, y, now);
    }
  }
  if (g_ui.screen() == Ui::kTuner && chain.tunerOn()) serviceTuner(now);
  g_ui.tick(now);
  Rect r;
  if (g_ui.render(g_strip, kStripPixels, &r)) g_tft.writeRect(r.x, r.y, r.w, r.h, g_strip);
}

// ---- Shell ----
void screenCommand(const char* arg) {
  static const char* const kScreens[] = {"home", "presets", "tuner", "question", "block settings", "files"};
  if (!arg || !*arg) {
    Serial.printf("Touch screen: %s, %s (%s), touch %s, showing %s.\n", g_present ? "display found" : "no display found",
                  g_active ? "on" : "off", g_rec.mode == kAuto ? "automatic" : "forced",
                  g_rec.calibrated ? "calibrated" : "not calibrated", kScreens[g_ui.screen()]);
    Serial.println("  tft on | off | auto   draw and read touch always, never, or when a display is found at boot\n"
                   "  tft cal               calibrate the touch panel (two crosses)\n"
                   "  tft tap <x> <y> [ms]  debug: a touch at a screen point, as a finger would (80 ms, or as given)\n"
                   "  tft touch             debug: touch readings for 10 s\n"
                   "  tft bench meters|full|off  debug: draw to measure what drawing costs the audio");
    return;
  }
  if (!strcmp(arg, "on") || !strcmp(arg, "off") || !strcmp(arg, "auto")) {
    g_rec.mode = !strcmp(arg, "on") ? kForcedOn : (!strcmp(arg, "off") ? kForcedOff : kAuto);
    saveRecord();
    activate(g_rec.mode == kForcedOn || (g_rec.mode == kAuto && g_present));
    Serial.printf("Touch screen %s.\n", g_active ? "on" : "off");
  } else if (!strcmp(arg, "cal")) {
    startCalibration();
    Serial.println("Touch each cross on the screen for a moment.");
  } else if (!strncmp(arg, "tap ", 4)) {
    char* end;
    g_tapX = (int)strtol(arg + 4, &end, 10);
    g_tapY = (int)strtol(end, &end, 10);
    const long ms = strtol(end, nullptr, 10);
    g_tapUntil = millis() + (ms > 0 && ms <= 10000 ? ms : 80);
    if (!g_active) Serial.println("The touch screen is off ('tft on').");
  } else if (!strcmp(arg, "touch")) {
    g_bench = kOff;
    g_rawUntil = millis() + 10000;
    Serial.println("Touch the screen: readings for 10 s.");
  } else if (!strcmp(arg, "bench off")) {
    g_bench = kOff;
    g_ui.invalidate();
    Serial.println("tft bench off.");
  } else if (!strcmp(arg, "bench full") || !strcmp(arg, "bench meters")) {
    g_bench = !strcmp(arg, "bench full") ? kFull : kMeters;
    g_reportMs = millis();
    g_frames = g_drawUs = g_worstUs = 0;
    Serial.printf("tft bench %s: a line every 5 s; read the audio's cost with 's'.\n",
                  g_bench == kFull ? "full (repainting all the time)" : "meters (two bars 25 times a second)");
  } else {
    Serial.println("Unknown: type 'tft' for its commands.");
  }
}
