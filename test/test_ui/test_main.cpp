// test_ui - the touch screen's portable code (lib/ui): drawing in strips, repainting only
// what changed, touch handling, and what each screen does to the unit (a fake one here).
// It also writes each screen as raw pixels to .pio/ui/, which tools/ui_render.py turns
// into PNG files for looking at.
#include <unity.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ui/Ui.h>

using namespace ui_tft;

void setUp() {}
void tearDown() {}

namespace {

struct FakeModel : UiModel {
  std::vector<std::string> names = {"01 Recto 91", "02 JCM8OO", "03 Clean Chorus", "04 Lead", "05 Plexi", "06 Tweed", "07 Djent"};
  int current = 1, saved = 1;
  bool edited = false, previewOn = false, editedBefore = false;
  bool on[kBlockCount] = {true, false, true, false, false, false, true, true, false};
  float inDb = -100, outDb = -100;
  int cpu = 57;
  bool tunerIsOpen = false;
  TunerView view;
  std::string log;  // what the screen asked for, in order

  int presetCount() override { return (int)names.size(); }
  int presetCurrent() override { return current; }
  int presetSaved() override { return previewOn ? saved : current; }
  const char* presetName(int idx) override { return names[idx].c_str(); }
  int presetNumber(int idx) override { return atoi(names[idx].c_str()); }
  bool presetEdited() override { return edited; }
  bool recall(int idx) override {
    log += "recall" + std::to_string(idx) + " ";
    current = saved = idx;
    edited = previewOn = false;
    return true;
  }
  bool preview(int idx) override {
    log += "preview" + std::to_string(idx) + " ";
    if (!previewOn) {
      saved = current;
      editedBefore = edited;
      previewOn = true;
    }
    current = idx;
    edited = idx == saved ? editedBefore : false;
    return true;
  }
  void cancelPreview() override {
    log += "cancel ";
    if (previewOn) {
      current = saved;
      edited = editedBefore;
    }
    previewOn = false;
  }
  void keepPreview() override {
    log += "keep ";
    previewOn = false;
    saved = current;
  }
  bool save() override {
    log += "save ";
    edited = false;
    return true;
  }
  bool blockOn(int b) override { return on[b]; }
  void toggleBlock(int b) override {
    log += "toggle" + std::to_string(b) + " ";
    on[b] = !on[b];
  }
  // Settings: every one 0 .. 100 in steps of 1, starting at 50, except the types and modes
  // (choices by name) and a few toggles. The rules for what is shown come from kBlocks.
  int val[kBlockCount][kMaxBlockSettings];
  FakeModel() {
    for (auto& b : val)
      for (int& v : b) v = 50;
    for (int b = 0; b < kBlockCount; b++)
      for (int i = 0; i < kBlocks[b].settingCount; i++)
        if (choices(kBlocks[b].settings[i].ref) || isToggle(kBlocks[b].settings[i].ref)) val[b][i] = 0;
    val[kBlockCount - 1][find(kBlockCount - 1, "drum style")] = 1;
    val[kBlockCount - 1][find(kBlockCount - 1, "drum kit")] = 1;
  }
  static const char* const* choices(const char* ref) {
    static const char* const dtype[] = {"digital", "tape", "analog", nullptr};
    static const char* const rmode[] = {"hall", "gated", "plate", "spring", nullptr};
    static const char* const mtype[] = {"chorus", "flanger", "ce2", "phase90", nullptr};
    static const char* const gtype[] = {"nam", "hard", nullptr};
    static const char* const style[] = {"metronome", "rock", "offbeat 8ths", "offbeat 16ths", "triplets", "pop", "blues shuffle", "6/8 ballad", nullptr};
    static const char* const song[] = {"off", "Back in Black-ish (built-in)", "Sweet Child-ish (built-in)", "Livin on a Prayer-ish (built-in)",
                                       "Enter Sandman-ish (built-in)", "Smoke on Water-ish (built-in)", nullptr};
    static const char* const kit[] = {"synth", "rock", nullptr};
    if (!strcmp(ref, "drum style")) return style;
    if (!strcmp(ref, "drum song")) return song;
    if (!strcmp(ref, "drum kit")) return kit;
    return !strcmp(ref, "dtype") ? dtype : (!strcmp(ref, "rmode") ? rmode : (!strcmp(ref, "mod type") ? mtype : (!strcmp(ref, "gate type") ? gtype : nullptr)));
  }
  static bool isToggle(const char* ref) {
    return !strcmp(ref, "amp cal") || !strcmp(ref, "lim look") || !strcmp(ref, "drum train") || !strcmp(ref, "bass");
  }
  static int find(int b, const char* ref) {
    for (int i = 0; i < kBlocks[b].settingCount; i++)
      if (!strcmp(kBlocks[b].settings[i].ref, ref)) return i;
    return -1;
  }
  bool setting(int b, int i, SettingView* v) override {
    const BlockSetting& st = kBlocks[b].settings[i];
    if (st.onlyRef) {
      for (int j = 0; j < kBlocks[b].settingCount; j++)
        if (!strcmp(kBlocks[b].settings[j].ref, st.onlyRef)) {
          const std::string want = std::string("|") + choices(st.onlyRef)[val[b][j]] + "|";
          if (!strstr(st.onlyChoices, want.c_str())) return false;
        }
    }
    snprintf(v->label, sizeof(v->label), "%s", st.ref);
    v->toggle = isToggle(st.ref);
    v->on = val[b][i] != 0;
    const char* const* c = choices(st.ref);
    v->bar = !v->toggle && !c;
    for (v->choices = 0; c && c[v->choices];) v->choices++;
    v->choice = val[b][i];
    v->fraction = val[b][i] / 100.0f;
    if (c) snprintf(v->text, sizeof(v->text), "%s", c[val[b][i]]);
    else if (v->toggle) snprintf(v->text, sizeof(v->text), "%s", v->on ? "ON" : "OFF");
    else snprintf(v->text, sizeof(v->text), "%d%%", val[b][i]);
    return true;
  }
  void settingStep(int b, int i, int steps) override {
    const char* ref = kBlocks[b].settings[i].ref;
    int hi = 100;
    if (isToggle(ref)) hi = 1;
    if (const char* const* c = choices(ref)) {
      hi = -1;
      while (c[hi + 1]) hi++;
    }
    int v = val[b][i] + steps;
    val[b][i] = v < 0 ? 0 : (v > hi ? hi : v);
    edited = true;
  }
  std::vector<std::string> models = {"built-in", "01 - CHUG NORRIS RAW", "JCM 800 2203 Amp Gain 3", "JCM 800 2203 Amp Gain 5",
                                     "JCM 800 2203 OD8008 Boosted Amp Gain 39", "TRCK SSS Clean BAL CAB", "TW TC DI Drive with BB"};
  std::vector<std::string> irs = {"OH 112 Deluxe OX12K", "OH 412 RECT", "OH Hair-Metal"};
  int model = 4, ir = 2;
  int fileCount(FileKind k) override { return (int)(k == kFileModel ? models : irs).size(); }
  int fileCurrent(FileKind k) override { return k == kFileModel ? model : ir; }
  const char* fileName(FileKind k, int idx) override { return (k == kFileModel ? models : irs)[idx].c_str(); }
  bool fileLoad(FileKind k, int idx) override {
    log += (k == kFileModel ? "model" : "ir") + std::to_string(idx) + " ";
    (k == kFileModel ? model : ir) = idx;
    edited = true;
    return true;
  }
  bool canSaveAs = true;
  bool rename(const char* name) override {
    log += std::string("rename[") + name + "] ";
    if (!name[0] || current < 0) return false;
    char file[64];
    snprintf(file, sizeof(file), "%02d %s", presetNumber(current), name);
    names[current] = file;
    return true;
  }
  bool remove() override {
    log += "remove" + std::to_string(current) + " ";
    if (current < 0) return false;
    names.erase(names.begin() + current);
    current = saved = -1;
    edited = false;
    return true;
  }
  bool saveAs(const char* name) override {
    log += std::string("saveAs[") + name + "] ";
    if (!canSaveAs) return false;
    char file[64];
    snprintf(file, sizeof(file), "%02d %s", (int)names.size() + 1, name[0] ? name : "Preset");
    names.push_back(file);
    current = saved = (int)names.size() - 1;
    edited = previewOn = false;
    return true;
  }
  const char* settingChoice(int b, int i, int idx) override { return choices(kBlocks[b].settings[i].ref)[idx]; }
  void settingPick(int b, int i, int idx) override {
    log += "pick" + std::to_string(idx) + " ";
    val[b][i] = idx;
    edited = true;
  }
  void settingSet(int b, int i, float fraction) override {
    val[b][i] = (int)(fraction * 100.0f + 0.5f);
    edited = true;
    sets++;
  }
  int sets = 0;
  void masterSet(float fraction) override { masterHalfDb = -120 + (int)(fraction * 120.0f + 0.5f); }
  int masterHalfDb = 0;  // -120 (silent) .. 0
  void master(SettingView* v) override {
    snprintf(v->label, sizeof(v->label), "Master");
    if (masterHalfDb <= -120) snprintf(v->text, sizeof(v->text), "silent");
    else snprintf(v->text, sizeof(v->text), "%.1f dB", masterHalfDb / 2.0);
    v->fraction = (masterHalfDb + 120) / 120.0f;
    v->bar = true;
  }
  void masterStep(int steps) override {
    masterHalfDb += steps;
    masterHalfDb = masterHalfDb < -120 ? -120 : (masterHalfDb > 0 ? 0 : masterHalfDb);
  }
  int tempoNow = 100;
  int drumTempoNow() override { return tempoNow; }
  float inputDb() override { return inDb; }
  float outputDb() override { return outDb; }
  int cpuPercent() override { return cpu; }
  void tunerOpen(bool o) override {
    log += o ? "tunerOn " : "tunerOff ";
    tunerIsOpen = o;
  }
  TunerView tuner() override { return view; }
};

// A stand-in for the display: collects strips into a frame and notes what was sent.
struct Glass {
  std::vector<uint16_t> px = std::vector<uint16_t>(kScreenW * kScreenH, 0xF81F);  // magenta: never painted
  int strips = 0, pixels = 0;
  int x0 = 999, y0 = 999, x1 = -1, y1 = -1;  // the bounds of what was sent

  // Paints until the screen says it is up to date. stripPixels: the strip buffer's size.
  void paint(Ui& ui, int stripPixels = kScreenW * 16) {
    std::vector<uint16_t> buf(stripPixels);
    Rect r;
    strips = pixels = 0;
    x0 = y0 = 999;
    x1 = y1 = -1;
    while (ui.render(buf.data(), stripPixels, &r)) {
      TEST_ASSERT_TRUE(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= kScreenW && r.y + r.h <= kScreenH);
      TEST_ASSERT_TRUE(r.w * r.h <= stripPixels);
      for (int y = 0; y < r.h; y++) memcpy(&px[(r.y + y) * kScreenW + r.x], &buf[y * r.w], r.w * 2);
      strips++;
      pixels += r.w * r.h;
      if (r.x < x0) x0 = r.x;
      if (r.y < y0) y0 = r.y;
      if (r.x + r.w > x1) x1 = r.x + r.w;
      if (r.y + r.h > y1) y1 = r.y + r.h;
      TEST_ASSERT_TRUE_MESSAGE(strips < 2000, "painting never ends");
    }
  }
  void save(const char* name) const {
    mkdir(".pio", 0755);
    mkdir(".pio/ui", 0755);
    const std::string path = std::string(".pio/ui/") + name + ".rgb565";
    FILE* f = fopen(path.c_str(), "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(px.data(), 2, px.size(), f);
    fclose(f);
  }
};

uint32_t g_ms = 1000;
// A tap: down for 80 ms, up for 80 ms.
void tap(Ui& ui, int x, int y) {
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(true, x, y, g_ms), ui.tick(g_ms);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms), ui.tick(g_ms);
}

// Places on the screens (the middle of each control; see Ui.cpp's layouts).
const int kPrevX = 28, kNextX = 292, kArrowY = 52, kNameX = 160, kNameY = 60;
int tileX(int i) { return 4 + (i % 5) * 63 + 30; }
int tileY(int i) { return 122 + (i / 5) * 44 + 20; }
int rowY(int row) { return 24 + row * 36 + 17; }
const int kTunerX = 40, kSaveBtnX = 114, kVolX = 283;  // the home screen's bottom line (with PRESETS at 240)
const int kBarY = 222, kBackX = 40, kUpX = 120, kDownX = 200, kOkX = 278;
const int kDlgY = 166, kSaveX = 58, kDiscardX = 160, kCancelX = 262;

void test_strip_size_does_not_change_the_picture() {
  FakeModel m;
  m.edited = true;
  Ui a(m), b(m), c(m);
  Glass ga, gb, gc;
  ga.paint(a, kScreenW * 16);
  gb.paint(b, kScreenW * 8);
  gc.paint(c, kScreenW * kScreenH);
  TEST_ASSERT_EQUAL_INT(15, ga.strips);
  TEST_ASSERT_EQUAL_INT(1, gc.strips);
  TEST_ASSERT_TRUE(ga.px == gb.px);
  TEST_ASSERT_TRUE(ga.px == gc.px);
  for (uint16_t v : ga.px) TEST_ASSERT_TRUE_MESSAGE(v != 0xF81F, "a pixel was never painted");
}

void test_only_what_changed_is_repainted() {
  FakeModel m;
  Ui ui(m);
  ui.tick(g_ms += 1000);  // the first readings of the meters and the CPU
  Glass g;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(kScreenW * kScreenH, g.pixels);
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.strips, "nothing changed, nothing sent");

  // A block switched from elsewhere (the app, MIDI): its tile alone.
  m.on[3] = true;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(60 * 40, g.pixels);
  TEST_ASSERT_EQUAL_INT(4 + 3 * 63, g.x0);
  TEST_ASSERT_EQUAL_INT(122, g.y0);

  // A level: only the meter's segments that change (5 x 10 each).
  m.inDb = -20;
  ui.tick(g_ms += 60);
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(6 * 50, g.pixels);  // the segments at -54 .. -24 light
  TEST_ASSERT_TRUE(g.y1 <= 20);
  m.inDb = -100;
  ui.tick(g_ms += 60);  // falls 1.5 dB a reading: no segment changes yet
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(0, g.pixels);

  // The same picture as a fresh screen would paint.
  for (int i = 0; i < 60; i++) ui.tick(g_ms += 60);
  g.paint(ui);
  Ui fresh(m);
  for (int i = 0; i < 2; i++) fresh.tick(g_ms + 1000 * (i + 1));
  Glass g2;
  g2.paint(fresh);
  TEST_ASSERT_TRUE(g.px == g2.px);
}

void test_home_steps_presets_and_toggles_blocks() {
  FakeModel m;
  Ui ui(m);
  tap(ui, kNextX, kArrowY);
  TEST_ASSERT_EQUAL_STRING("recall2 ", m.log.c_str());
  tap(ui, kPrevX, kArrowY);
  tap(ui, kPrevX, kArrowY);
  TEST_ASSERT_EQUAL_INT(0, m.current);
  tap(ui, kPrevX, kArrowY);
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, m.current, "previous from the first goes to the last");
  m.log.clear();
  tap(ui, tileX(1), tileY(1));
  tap(ui, tileX(8), tileY(8));
  TEST_ASSERT_EQUAL_STRING("toggle1 toggle8 ", m.log.c_str());
  m.log.clear();
  tap(ui, 160, 119);  // between the name and the tiles
  tap(ui, 65, 140);   // between two tiles
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.log.c_str(), "a tap between controls does nothing");
}

void test_touch_bounce_slide_and_hold() {
  FakeModel m;
  Ui ui(m);
  // Contact lost for 20 ms in the middle of a press: still one tap.
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, kNextX, kArrowY, g_ms);
  for (int i = 0; i < 2; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms);
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, kNextX + 3, kArrowY - 2, g_ms);
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms);
  TEST_ASSERT_EQUAL_STRING("recall2 ", m.log.c_str());

  // Sliding off before letting go: nothing happens.
  m.log.clear();
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, kNextX, kArrowY, g_ms);
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, 160, 150, g_ms);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms);
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());

  // Holding next: the first step after 0.6 s, then one every 0.3 s, none on release.
  for (int t = 0; t <= 1300; t += 10, g_ms += 10) ui.touch(true, kNextX, kArrowY, g_ms), ui.tick(g_ms);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms), ui.tick(g_ms);
  TEST_ASSERT_EQUAL_STRING("recall3 recall4 recall5 ", m.log.c_str());

  // A pressed button is drawn pressed, and normal again afterwards.
  Glass g;
  g.paint(ui);
  ui.touch(true, kPrevX, kArrowY, g_ms += 100);
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(48 * 60, g.pixels);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms);
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels > 48 * 60);  // the button and what the new preset changed
}

void test_leaving_an_edited_preset_asks() {
  FakeModel m;
  m.edited = true;
  Ui ui(m);
  tap(ui, kNextX, kArrowY);
  TEST_ASSERT_EQUAL_INT(Ui::kConfirm, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, kCancelX, kDlgY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  TEST_ASSERT_TRUE(m.edited);

  tap(ui, kNextX, kArrowY);
  tap(ui, kSaveX, kDlgY);
  TEST_ASSERT_EQUAL_STRING("save recall2 ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());

  m.log.clear();
  m.edited = true;
  tap(ui, kPrevX, kArrowY);
  tap(ui, kDiscardX, kDlgY);
  TEST_ASSERT_EQUAL_STRING("recall1 ", m.log.c_str());
}

void test_preset_list_previews_keeps_and_goes_back() {
  FakeModel m;
  Ui ui(m);
  tap(ui, kNameX, kNameY);
  TEST_ASSERT_EQUAL_INT(Ui::kPresets, ui.screen());
  tap(ui, 160, rowY(3));
  tap(ui, 160, rowY(0));
  TEST_ASSERT_EQUAL_STRING("preview3 preview0 ", m.log.c_str());
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_STRING("preview3 preview0 cancel ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(1, m.current);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());

  // The second page, a row on it, OK.
  m.log.clear();
  tap(ui, 240, kBarY);  // the PRESETS button
  tap(ui, kDownX, kBarY);
  tap(ui, kUpX, kBarY);
  tap(ui, kUpX, kBarY);  // no page before the first
  tap(ui, kDownX, kBarY);
  tap(ui, 160, rowY(1));
  tap(ui, 160, rowY(4));  // no preset in this row
  tap(ui, kDownX, kBarY);  // no third page
  tap(ui, kOkX, kBarY);
  TEST_ASSERT_EQUAL_STRING("preview6 keep ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(6, m.current);

  // Opened and left without touching a row: nothing is asked of the unit.
  m.log.clear();
  tap(ui, kNameX, kNameY);
  TEST_ASSERT_EQUAL_INT(Ui::kPresets, ui.screen());
  tap(ui, kOkX, kBarY);
  tap(ui, kNameX, kNameY);
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
}

void test_preset_list_with_unsaved_edits() {
  FakeModel m;
  m.edited = true;
  Ui ui(m);
  tap(ui, kNameX, kNameY);
  tap(ui, 160, rowY(3));
  // Back: the edited sound returns.
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_TRUE(m.edited && m.current == 1);

  // OK on another preset: asks; Save saves the edited sound first, then recalls the choice.
  m.log.clear();
  tap(ui, kNameX, kNameY);
  tap(ui, 160, rowY(3));
  tap(ui, kOkX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kConfirm, ui.screen());
  tap(ui, kCancelX, kDlgY);
  TEST_ASSERT_EQUAL_INT_MESSAGE(Ui::kPresets, ui.screen(), "Cancel returns to the list");
  tap(ui, kOkX, kBarY);
  tap(ui, kSaveX, kDlgY);
  TEST_ASSERT_EQUAL_STRING("preview3 cancel save recall3 ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(3, m.current);

  // Discard keeps what plays.
  m.log.clear();
  m.edited = true;
  tap(ui, kNameX, kNameY);
  tap(ui, 160, rowY(0));
  tap(ui, kOkX, kBarY);
  tap(ui, kDiscardX, kDlgY);
  TEST_ASSERT_EQUAL_STRING("preview0 keep ", m.log.c_str());

  // Back on the preset from before (its edits return with it): OK needs no question.
  m.log.clear();
  m.edited = true;
  tap(ui, kNameX, kNameY);
  tap(ui, 160, rowY(2));
  tap(ui, 160, rowY(0));
  tap(ui, kOkX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_TRUE(m.edited);
}

// Places on a block's settings screen.
const int kMinusX = 28, kMidX = 160, kPlusX = 292, kEditTopY = 16, kEditBackX = 36, kEditOnX = 276;
int setY(int row) { return 34 + row * 43 + 20; }
void hold(Ui& ui, int x, int y, int ms) {
  for (int t = 0; t <= ms; t += 10, g_ms += 10) ui.touch(true, x, y, g_ms), ui.tick(g_ms);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms), ui.tick(g_ms);
}

void test_block_settings() {
  FakeModel m;
  Ui ui(m);
  const int kDelay = 5;
  // Holding a tile opens its settings and does not switch the block; a tap still switches.
  hold(ui, tileX(kDelay), tileY(kDelay), 700);
  TEST_ASSERT_EQUAL_INT(Ui::kBlockEdit, ui.screen());
  TEST_ASSERT_EQUAL_INT(kDelay, ui.editedBlock());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  TEST_ASSERT_FALSE(m.on[kDelay]);

  // Rows: dtype, dmix, dtime, dfb. Plus and minus step once a tap.
  tap(ui, kPlusX, setY(1));
  tap(ui, kPlusX, setY(1));
  tap(ui, kMinusX, setY(2));
  TEST_ASSERT_EQUAL_INT(52, m.val[kDelay][1]);
  TEST_ASSERT_EQUAL_INT(49, m.val[kDelay][2]);
  TEST_ASSERT_TRUE(m.edited);

  // Holding plus: single steps from 0.4 s every 0.1 s, five at a time after the tenth.
  hold(ui, kPlusX, setY(3), 900);  // steps at 400 .. 900 ms: six
  TEST_ASSERT_EQUAL_INT(56, m.val[kDelay][3]);
  m.val[kDelay][3] = 0;
  hold(ui, kPlusX, setY(3), 1900);  // sixteen repeats: ten single, six of five
  TEST_ASSERT_EQUAL_INT(40, m.val[kDelay][3]);

  // The type (row 0) changes which settings there are: digital has nine (three pages),
  // analog six (two pages); the ones that do not apply are gone, the rest close up.
  int painted;
  Glass g;
  g.paint(ui);
  tap(ui, 260, 222);  // down
  tap(ui, 260, 222);  // down: page 3 of 3 (dera alone)
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT_MESSAGE(51, m.val[kDelay][8], "page 3 shows dera in its first row");
  tap(ui, 50, 222);  // up
  tap(ui, 50, 222);
  tap(ui, kPlusX, setY(0));
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(2, m.val[kDelay][0]);  // analog
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, m.val[kDelay][0], "the last choice is the end");
  tap(ui, 260, 222);  // page 2 of 2: dtone, dmod
  tap(ui, kPlusX, setY(1));
  TEST_ASSERT_EQUAL_INT(51, m.val[kDelay][5]);
  tap(ui, kPlusX, setY(2));  // nothing in this row
  tap(ui, 260, 222);         // no third page now
  tap(ui, kPlusX, setY(1));
  TEST_ASSERT_EQUAL_INT(52, m.val[kDelay][5]);
  g.paint(ui);
  painted = g.pixels;
  g.paint(ui);
  TEST_ASSERT_TRUE(painted > 0 && g.pixels == 0);

  // ON / OFF at the top switches the block; BACK returns home.
  m.log.clear();
  tap(ui, kEditOnX, kEditTopY);
  TEST_ASSERT_EQUAL_STRING("toggle5 ", m.log.c_str());
  tap(ui, kEditBackX, kEditTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());

  // A toggle: plus is on, minus is off, a tap on its name flips. (The amp's 7th: amp cal,
  // on its second page in the last row, because the amp's first row is its model.)
  hold(ui, tileX(0), tileY(0), 700);
  tap(ui, 260, 222);
  TEST_ASSERT_EQUAL_INT(0, m.val[0][6]);
  tap(ui, kMidX, setY(3));
  TEST_ASSERT_EQUAL_INT(1, m.val[0][6]);
  tap(ui, kPlusX, setY(3));
  TEST_ASSERT_EQUAL_INT(1, m.val[0][6]);
  tap(ui, kMinusX, setY(3));
  TEST_ASSERT_EQUAL_INT(0, m.val[0][6]);
  tap(ui, kMidX, setY(2));  // a number: a tap on its name does nothing
  TEST_ASSERT_EQUAL_INT(50, m.val[0][5]);
  tap(ui, kEditBackX, kEditTopY);

}

// A finger put down at (x0, y), slid to (x1, y1) in steps of 2 pixels and lifted. The last
// reading before the lift is at (liftX, y1): a resistive panel's point jumps there.
void drag(Ui& ui, int x0, int y, int x1, int y1, int liftX = -1) {
  const int n = (x1 > x0 ? x1 - x0 : x0 - x1) / 2;
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, x0, y, g_ms), ui.tick(g_ms);
  for (int i = 1; i <= n; i++, g_ms += 10) ui.touch(true, x0 + (x1 - x0) * i / n, y + (y1 - y) * i / n, g_ms), ui.tick(g_ms);
  for (int i = 0; i < 5; i++, g_ms += 10) ui.touch(true, x1, y1, g_ms), ui.tick(g_ms);
  if (liftX >= 0) ui.touch(true, liftX, y1, g_ms), ui.tick(g_ms), g_ms += 10;
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms), ui.tick(g_ms);
}

void test_dragging_a_value() {
  FakeModel m;
  Ui ui(m);
  const int kDelay = 5;
  hold(ui, tileX(kDelay), tileY(kDelay), 700);
  // Rows: dtype (a choice), dmix, dtime, dfb; all numbers at 50 of 0 .. 100, the bar 192 px.
  // The first 10 px begin the drag, the rest move the value: 58 px = 48 px = 25 of 100.
  drag(ui, 120, setY(1), 178, setY(1));
  TEST_ASSERT_EQUAL_INT(75, m.val[kDelay][1]);
  TEST_ASSERT_TRUE(m.edited);
  drag(ui, 200, setY(1), 104, setY(1));  // back by 86 px = 45
  TEST_ASSERT_EQUAL_INT(30, m.val[kDelay][1]);
  // Past the ends it stays at the end, and comes back with the finger.
  drag(ui, 100, setY(2), 310, setY(2));
  TEST_ASSERT_EQUAL_INT(100, m.val[kDelay][2]);
  drag(ui, 250, setY(2), 60, setY(2));
  TEST_ASSERT_EQUAL_INT(6, m.val[kDelay][2]);  // 100 less 180 px of 192
  // Once begun, the finger may leave the row: the other rows are not touched.
  drag(ui, 100, setY(3), 148, setY(1));
  TEST_ASSERT_EQUAL_INT(70, m.val[kDelay][3]);
  TEST_ASSERT_EQUAL_INT(30, m.val[kDelay][1]);
  // A tap, and a finger that wanders less than 10 px, change nothing.
  m.sets = 0;
  tap(ui, kMidX, setY(1));
  drag(ui, 150, setY(1), 158, setY(1));
  TEST_ASSERT_EQUAL_INT(0, m.sets);
  // The jump of the last reading as the finger lifts is dropped.
  drag(ui, 100, setY(1), 130, setY(1), 250);
  TEST_ASSERT_EQUAL_INT(40, m.val[kDelay][1]);
  // A choice is not dragged, and sliding off minus or plus still only lets the button go.
  drag(ui, 100, setY(0), 200, setY(0));
  TEST_ASSERT_EQUAL_INT(0, m.val[kDelay][0]);
  TEST_ASSERT_EQUAL_INT_MESSAGE(Ui::kChoices, ui.screen(), "a touch that ends on a choice opens its list");
  tap(ui, 50, 222);
  drag(ui, kPlusX, setY(1), 150, setY(1));
  TEST_ASSERT_EQUAL_INT(40, m.val[kDelay][1]);
  // What changed is repainted: the row, nothing else.
  Glass g;
  g.paint(ui);
  drag(ui, 100, setY(1), 130, setY(1));
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels > 0 && g.pixels <= 208 * 40);
  tap(ui, kEditBackX, kEditTopY);

  // The master volume: its bar is 280 px for 120 half-dB steps.
  tap(ui, kVolX, kBarY);
  drag(ui, 250, 70, 120, 100);  // 120 px left = 51 steps
  TEST_ASSERT_EQUAL_INT(-51, m.masterHalfDb);
  drag(ui, 30, 100, 310, 100);
  TEST_ASSERT_EQUAL_INT(0, m.masterHalfDb);
  tap(ui, 160, 70);
  TEST_ASSERT_EQUAL_INT(0, m.masterHalfDb);
  tap(ui, 36, 16);

  // The trainer's rows are dragged the same way.
  const int drums = kBlockCount - 1;
  tap(ui, 4 + 4 * 63 + 30, 122 + 44 + 20);
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  drag(ui, 120, setY(0), 178, setY(0));
  TEST_ASSERT_EQUAL_INT(75, m.val[drums][FakeModel::find(drums, "bpm")]);
}

// Places on the save menu and the keyboard.
const int kMenuX = 160, kOverY = 66, kNewY = 110, kMenuCancelY = 208, kRenameX = 89, kDeleteX = 231, kMenuRowY = 154;
void key(Ui& ui, int row, int col) { tap(ui, 4 + col * 313 / 10 + 14, 36 + row * 42 + 20); }
void type(Ui& ui, const char* text) {
  static const char* const rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM"};
  for (; *text; text++) {
    if (*text == ' ') {
      key(ui, 3, 8);
      continue;
    }
    for (int row = 0; row < 4; row++)
      for (int col = 0; rows[row][col]; col++)
        if (rows[row][col] == (*text >= 'a' && *text <= 'z' ? *text - 32 : *text)) key(ui, row, col);
  }
}

void test_save_over_and_as_new() {
  FakeModel m;
  Ui ui(m);
  // Nothing changed: saving over is not offered, a new preset is.
  tap(ui, kSaveBtnX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kSaveMenu, ui.screen());
  tap(ui, kMenuX, kOverY);
  TEST_ASSERT_EQUAL_INT(Ui::kSaveMenu, ui.screen());
  tap(ui, kMenuX, kMenuCancelY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());

  // Edited: over the recalled preset.
  m.edited = true;
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kOverY);
  TEST_ASSERT_EQUAL_STRING("save ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());

  // As new: the first letter of a word is upper case, the rest lower; Aa changes that for
  // what follows; delete takes the last character; spaces at the end are dropped.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  type(ui, "blues lead x");
  key(ui, 3, 9);  // delete the x
  key(ui, 3, 7);  // lower case from here
  type(ui, "jcm");
  key(ui, 3, 7);  // and upper
  type(ui, "x800 ");
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("saveAs[Blues Lead jcmX800] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("08 Blues Lead jcmX800", m.names[m.current].c_str());

  // No more than 24 characters; no name at all is allowed; Cancel saves nothing.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  type(ui, "123456789012345678901234567890");
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("saveAs[123456789012345678901234] ", m.log.c_str());
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("saveAs[] ", m.log.c_str());
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  type(ui, "abc");
  tap(ui, 80, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());

  // The unit cannot save (no card): the screen stays and says so; typing clears the message.
  m.canSaveAs = false;
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  type(ui, "a");
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  Glass g;
  g.paint(ui);
  type(ui, "b");
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels >= 312 * 30);
}

void test_rename_and_delete() {
  FakeModel m;
  Ui ui(m);
  // Rename starts from the name the preset has; its number stays. Edits are not touched.
  m.edited = true;
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kRenameX, kMenuRowY);
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  type(ui, " x");  // after a space the next letter is upper case
  key(ui, 3, 9);
  type(ui, "live");
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("rename[JCM8OO Live] ", m.log.c_str());
  TEST_ASSERT_EQUAL_STRING("02 JCM8OO Live", m.names[1].c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_TRUE(m.edited);
  TEST_ASSERT_EQUAL_INT(7, (int)m.names.size());

  // An empty name is refused: the screen stays and says so. Cancel renames nothing.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kRenameX, kMenuRowY);
  for (int i = 0; i < 11; i++) key(ui, 3, 9);
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("rename[] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  tap(ui, 80, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("02 JCM8OO Live", m.names[1].c_str());

  // A new preset after a rename starts with an empty name and saves, not renames.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  type(ui, "a");
  tap(ui, 240, kBarY);
  TEST_ASSERT_EQUAL_STRING("saveAs[A] ", m.log.c_str());
  m.recall(1);

  // Delete asks first; Cancel deletes nothing.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kDeleteX, kMenuRowY);
  TEST_ASSERT_EQUAL_INT(Ui::kDeleteAsk, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, 232, 166);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kDeleteX, kMenuRowY);
  tap(ui, 88, 166);
  TEST_ASSERT_EQUAL_STRING("remove1 ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_EQUAL_INT(-1, m.current);

  // With no preset recalled there is nothing to rename, delete or save over.
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kRenameX, kMenuRowY);
  tap(ui, kDeleteX, kMenuRowY);
  tap(ui, kMenuX, kOverY);
  TEST_ASSERT_EQUAL_INT(Ui::kSaveMenu, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, kMenuX, kMenuCancelY);
}

void test_choices_have_a_list() {
  FakeModel m;
  Ui ui(m);
  const int kDrums = kBlockCount - 1;
  const int style = FakeModel::find(kDrums, "drum style"), song = FakeModel::find(kDrums, "drum song"), kit = FakeModel::find(kDrums, "drum kit");
  // The trainer's second page: a tap on the style opens its eight names, two pages, on
  // the page of the one that is set. A row chooses at once and the list stays.
  tap(ui, tileX(9), tileY(8));
  tap(ui, 282, 222);
  m.val[kDrums][style] = 6;
  tap(ui, kMidX, setY(0));
  TEST_ASSERT_EQUAL_INT(Ui::kChoices, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, 160, rowY(1));  // page 2: 5, 6, 7; this one is set
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.log.c_str(), "the one that is set is not set again");
  tap(ui, 160, rowY(2));
  tap(ui, 160, rowY(4));  // nothing in this row
  tap(ui, 260, kBarY);    // no third page
  tap(ui, 160, kBarY);    // up
  tap(ui, 160, kBarY);
  tap(ui, 160, rowY(3));
  TEST_ASSERT_EQUAL_STRING("pick7 pick3 ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(3, m.val[kDrums][style]);
  TEST_ASSERT_EQUAL_INT(Ui::kChoices, ui.screen());
  // Only the two rows that changed are repainted.
  Glass g;
  g.paint(ui);
  tap(ui, 160, rowY(0));
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(2 * 312 * 34, g.pixels);
  // BACK returns to where it was opened: the trainer's second page.
  tap(ui, 50, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(1, m.val[kDrums][style]);
  // The song's list begins with "off".
  tap(ui, kMidX, setY(1));
  tap(ui, 160, rowY(2));
  TEST_ASSERT_EQUAL_INT(2, m.val[kDrums][song]);
  tap(ui, 160, rowY(0));
  TEST_ASSERT_EQUAL_INT(0, m.val[kDrums][song]);
  tap(ui, 50, kBarY);
  // Two choices have no list: minus and plus are as quick.
  tap(ui, kMidX, setY(2));
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  TEST_ASSERT_EQUAL_INT(1, m.val[kDrums][kit]);
  tap(ui, kEditBackX, kEditTopY);

  // A block's type has one too, and BACK returns to the block's settings.
  const int kDelay = 5;
  hold(ui, tileX(kDelay), tileY(kDelay), 700);
  tap(ui, kMidX, setY(0));
  TEST_ASSERT_EQUAL_INT(Ui::kChoices, ui.screen());
  tap(ui, 160, rowY(2));
  TEST_ASSERT_EQUAL_INT(2, m.val[kDelay][0]);
  tap(ui, 50, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kBlockEdit, ui.screen());
}

void test_master_volume() {
  FakeModel m;
  Ui ui(m);
  tap(ui, kVolX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kMaster, ui.screen());
  tap(ui, 87, 152);
  tap(ui, 87, 152);
  TEST_ASSERT_EQUAL_INT(-2, m.masterHalfDb);
  tap(ui, 233, 152);
  tap(ui, 233, 152);
  tap(ui, 233, 152);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, m.masterHalfDb, "0 dB is the top");
  hold(ui, 87, 152, 1900);  // ten single steps, then six of five
  TEST_ASSERT_EQUAL_INT(-40, m.masterHalfDb);
  TEST_ASSERT_FALSE_MESSAGE(m.edited, "the master volume is not part of a preset");
  tap(ui, 36, 16);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
}

void test_choosing_a_model_and_an_ir() {
  FakeModel m;
  Ui ui(m);
  // The amp's first row is its model: the arrows step through the list, round the ends.
  hold(ui, tileX(0), tileY(0), 700);
  tap(ui, kPlusX, setY(0));
  tap(ui, kPlusX, setY(0));
  tap(ui, kPlusX, setY(0));
  tap(ui, kMinusX, setY(0));
  TEST_ASSERT_EQUAL_STRING("model5 model6 model0 model6 ", m.log.c_str());
  tap(ui, kPlusX, setY(1));  // the row after it is the first setting
  TEST_ASSERT_EQUAL_INT(51, m.val[0][0]);

  // A tap on the name opens the list on the page of the one that plays; a row loads at once.
  m.log.clear();
  tap(ui, kMidX, setY(0));
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  tap(ui, 160, rowY(1));  // page 2: models 5 and 6; this is the one that plays
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.log.c_str(), "the one that plays is not loaded again");
  tap(ui, 160, rowY(0));
  tap(ui, 160, rowY(3));   // no file in this row
  tap(ui, 160, kBarY);     // up
  tap(ui, 160, rowY(2));
  TEST_ASSERT_EQUAL_STRING("model5 model2 ", m.log.c_str());
  tap(ui, 40, kBarY);      // back to the amp's settings
  TEST_ASSERT_EQUAL_INT(Ui::kBlockEdit, ui.screen());
  TEST_ASSERT_EQUAL_INT(0, ui.editedBlock());
  tap(ui, kEditBackX, kEditTopY);

  // The cab has its IR the same way; a block without a file starts with its settings.
  m.log.clear();
  hold(ui, tileX(2), tileY(2), 700);
  tap(ui, kMinusX, setY(0));
  tap(ui, kMidX, setY(0));
  tap(ui, 160, rowY(0));
  TEST_ASSERT_EQUAL_STRING("ir1 ir0 ", m.log.c_str());
  tap(ui, 40, kBarY);
  tap(ui, kEditBackX, kEditTopY);
  hold(ui, tileX(3), tileY(3), 700);  // the doubler
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(51, m.val[3][0]);
}

void test_speed_trainer_is_one_tap_from_home() {
  FakeModel m;
  Ui ui(m);
  const int kDrums = kBlockCount - 1, kTrainX = tileX(9), kTrainY = tileY(8);
  const int on = FakeModel::find(kDrums, "drum train"), bpm = FakeModel::find(kDrums, "bpm");
  const int step = FakeModel::find(kDrums, "train step"), every = FakeModel::find(kDrums, "train every"),
            target = FakeModel::find(kDrums, "train target");
  TEST_ASSERT_TRUE(on >= 0 && bpm >= 0 && step >= 0 && every >= 0 && target >= 0);

  tap(ui, kTrainX, kTrainY);
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.log.c_str(), "opening it changes nothing");

  // Its four rows on one page: start tempo, step, every, target.
  tap(ui, kPlusX, setY(0));
  tap(ui, kMinusX, setY(1));
  tap(ui, kPlusX, setY(2));
  tap(ui, kPlusX, setY(3));
  hold(ui, kPlusX, setY(3), 900);
  TEST_ASSERT_EQUAL_INT(51, m.val[kDrums][bpm]);
  TEST_ASSERT_EQUAL_INT(49, m.val[kDrums][step]);
  TEST_ASSERT_EQUAL_INT(51, m.val[kDrums][every]);
  TEST_ASSERT_EQUAL_INT(57, m.val[kDrums][target]);

  // ON / OFF at the top is the trainer's switch; PLAY / STOP the drums'.
  tap(ui, kEditOnX, kEditTopY);
  TEST_ASSERT_EQUAL_INT(1, m.val[kDrums][on]);
  tap(ui, kEditOnX, kEditTopY);
  TEST_ASSERT_EQUAL_INT(0, m.val[kDrums][on]);
  tap(ui, kEditOnX, kEditTopY);
  tap(ui, 60, 222);
  TEST_ASSERT_TRUE(m.on[kDrums]);
  TEST_ASSERT_EQUAL_STRING("toggle8 ", m.log.c_str());

  // While it plays the tempo it has reached shows, here and on the home tile; only that
  // part is repainted when it changes.
  Glass g;
  g.paint(ui);
  m.tempoNow = 105;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(140 * 34, g.pixels);
  tap(ui, kEditBackX, kEditTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  g.paint(ui);
  m.tempoNow = 110;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(60 * 40, g.pixels);
  tap(ui, kTrainX, kTrainY);
  tap(ui, 60, 222);
  TEST_ASSERT_FALSE(m.on[kDrums]);

  // The second page is what the drums play: style, song, kit, volume. PLAY and the
  // trainer's switch stay; opening the trainer again shows its first page.
  const int style = FakeModel::find(kDrums, "drum style"), song = FakeModel::find(kDrums, "drum song"),
            kit = FakeModel::find(kDrums, "drum kit"), vol = FakeModel::find(kDrums, "drum vol");
  TEST_ASSERT_TRUE(style >= 0 && song >= 0 && kit >= 0 && vol >= 0);
  tap(ui, 282, 222);
  tap(ui, kPlusX, setY(0));
  tap(ui, kPlusX, setY(1));
  tap(ui, kMinusX, setY(2));
  tap(ui, kPlusX, setY(3));
  TEST_ASSERT_EQUAL_INT(2, m.val[kDrums][style]);
  TEST_ASSERT_EQUAL_INT(1, m.val[kDrums][song]);
  TEST_ASSERT_EQUAL_INT(0, m.val[kDrums][kit]);
  tap(ui, kMinusX, setY(1));
  TEST_ASSERT_EQUAL_INT(51, m.val[kDrums][vol]);
  TEST_ASSERT_EQUAL_INT_MESSAGE(51, m.val[kDrums][bpm], "the first page's rows are not touched");
  tap(ui, 60, 222);
  TEST_ASSERT_TRUE(m.on[kDrums]);
  tap(ui, 60, 222);
  tap(ui, 282, 222);
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(52, m.val[kDrums][bpm]);
  tap(ui, 282, 222);
  tap(ui, kEditBackX, kEditTopY);
  tap(ui, kTrainX, kTrainY);
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(53, m.val[kDrums][bpm]);
}

void test_tuner_opens_and_closes() {
  FakeModel m;
  Ui ui(m);
  tap(ui, kTunerX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kTuner, ui.screen());
  TEST_ASSERT_TRUE(m.tunerIsOpen);
  Glass g;
  g.paint(ui);
  m.view = {true, 9, 2, -12.4f, 109.2f};
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels > 0 && g.pixels < kScreenW * kScreenH / 2);
  m.view.cents = -12.2f;  // the same whole cent, the same Hz text: nothing to paint
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(0, g.pixels);
  tap(ui, 160, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  TEST_ASSERT_FALSE(m.tunerIsOpen);
  TEST_ASSERT_EQUAL_STRING("tunerOn tunerOff ", m.log.c_str());
}

void test_long_names_fit() {
  char out[64];
  TEST_ASSERT_FALSE(Canvas::fitText(kFontMedium, "Lead", 200, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Lead", out);
  TEST_ASSERT_TRUE(Canvas::fitText(kFontMedium, "JCM 800 2203 OD8008 Boosted Amp Gain 39", 200, out, sizeof(out)));
  TEST_ASSERT_TRUE(Canvas::textWidth(kFontMedium, out) <= 200);
  TEST_ASSERT_EQUAL_STRING("..", out + strlen(out) - 2);
  TEST_ASSERT_TRUE(strlen(out) > 12);
  TEST_ASSERT_TRUE(Canvas::fitText(kFontMedium, "Wide", 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  // The longest name a preset may have (24 characters, the widest letters) fits the home
  // screen in the medium font without being cut.
  TEST_ASSERT_TRUE(Canvas::textWidth(kFontMedium, "WWWWWWWWWWWWWWWWWWWWWWWW") > 312);  // only this one would not
  TEST_ASSERT_TRUE(Canvas::textWidth(kFontMedium, "Preset With A Long Name!") <= 312);
  // The huge font has the digits and note names only.
  TEST_ASSERT_NOT_NULL(kFontHuge.glyph('7'));
  TEST_ASSERT_NOT_NULL(kFontHuge.glyph('#'));
  TEST_ASSERT_NULL(kFontHuge.glyph('x'));
}

void test_blocks_header_matches_blocks_json() {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, system("python3 tools/make_blocks_header.py --check > /dev/null"),
                                "lib/ui/src/ui/BlocksGen.h is out of date: run tools/make_blocks_header.py");
  TEST_ASSERT_EQUAL_INT(9, kBlockCount);
  TEST_ASSERT_EQUAL_STRING("amp", kBlocks[0].id);
  TEST_ASSERT_TRUE(kBlocks[2].onInverted);  // the cab's switch is a bypass
}

// Not a check: the pictures for tools/ui_render.py.
void test_write_pictures() {
  {
    FakeModel m;
    m.inDb = -14;
    m.outDb = -5;
    Ui ui(m);
    ui.tick(g_ms += 1000);
    Glass g;
    g.paint(ui);
    g.save("home");
    m.names[1] = "02 Preset With A Long Name!";
    m.edited = true;
    m.on[1] = m.on[4] = m.on[5] = m.on[8] = true;
    m.outDb = -0.5f;
    ui.tick(g_ms += 1000);
    ui.touch(true, tileX(3), tileY(3), g_ms);
    g.paint(ui);
    g.save("home_edited");
  }
  {
    FakeModel m;
    Ui ui(m);
    tap(ui, kNameX, kNameY);
    tap(ui, 160, rowY(3));
    Glass g;
    g.paint(ui);
    g.save("presets");
  }
  {
    FakeModel m;
    Ui ui(m);
    tap(ui, kTunerX, kBarY);
    Glass g;
    g.paint(ui);
    g.save("tuner_idle");
    m.view = {true, 6, 2, -17.0f, 91.6f};
    g.paint(ui);
    g.save("tuner_flat");
    m.view = {true, 9, 2, 1.0f, 110.1f};
    g.paint(ui);
    g.save("tuner_in_tune");
  }
  {
    FakeModel m;
    m.edited = true;
    Ui ui(m);
    tap(ui, kNextX, kArrowY);
    Glass g;
    g.paint(ui);
    g.save("confirm");
  }
  {
    FakeModel m;
    m.on[5] = true;
    m.val[5][1] = 25;
    m.val[5][3] = 35;
    Ui ui(m);
    hold(ui, tileX(5), tileY(5), 700);
    Glass g;
    g.paint(ui);
    g.save("block_delay");
    tap(ui, kEditBackX, kEditTopY);
    hold(ui, tileX(0), tileY(0), 700);
    g.paint(ui);
    g.save("block_amp");
    tap(ui, kMidX, setY(0));
    g.paint(ui);
    g.save("files_models");
  }
  {
    FakeModel m;
    const int d = kBlockCount - 1;
    m.val[d][FakeModel::find(d, "drum train")] = 1;
    m.on[d] = true;
    m.tempoNow = 112;
    Ui ui(m);
    ui.tick(g_ms += 1000);
    Glass g;
    g.paint(ui);
    g.save("home_trainer");
    tap(ui, tileX(9), tileY(8));
    g.paint(ui);
    g.save("trainer");
    tap(ui, 282, 222);
    g.paint(ui);
    g.save("trainer_drums");
    tap(ui, kMidX, setY(1));
    g.paint(ui);
    g.save("choices_song");
  }
  {
    FakeModel m;
    m.edited = true;
    Ui ui(m);
    tap(ui, kSaveBtnX, kBarY);
    Glass g;
    g.paint(ui);
    g.save("save_menu");
    tap(ui, kMenuX, kNewY);
    type(ui, "blues lead");
    g.paint(ui);
    g.save("name");
    tap(ui, 80, kBarY);
    tap(ui, kVolX, kBarY);
    m.masterHalfDb = -12;
    g.paint(ui);
    g.save("master");
    tap(ui, 36, 16);
    tap(ui, kSaveBtnX, kBarY);
    tap(ui, kDeleteX, kMenuRowY);
    g.paint(ui);
    g.save("delete");
  }
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_strip_size_does_not_change_the_picture);
  RUN_TEST(test_only_what_changed_is_repainted);
  RUN_TEST(test_home_steps_presets_and_toggles_blocks);
  RUN_TEST(test_touch_bounce_slide_and_hold);
  RUN_TEST(test_leaving_an_edited_preset_asks);
  RUN_TEST(test_preset_list_previews_keeps_and_goes_back);
  RUN_TEST(test_preset_list_with_unsaved_edits);
  RUN_TEST(test_block_settings);
  RUN_TEST(test_choosing_a_model_and_an_ir);
  RUN_TEST(test_speed_trainer_is_one_tap_from_home);
  RUN_TEST(test_save_over_and_as_new);
  RUN_TEST(test_rename_and_delete);
  RUN_TEST(test_choices_have_a_list);
  RUN_TEST(test_master_volume);
  RUN_TEST(test_dragging_a_value);
  RUN_TEST(test_tuner_opens_and_closes);
  RUN_TEST(test_long_names_fit);
  RUN_TEST(test_blocks_header_matches_blocks_json);
  RUN_TEST(test_write_pictures);
  return UNITY_END();
}
