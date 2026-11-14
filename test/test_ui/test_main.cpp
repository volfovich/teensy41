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
  tap(ui, tileX(9), tileY(8));  // the wide tile's right half
  TEST_ASSERT_EQUAL_STRING("toggle1 toggle8 toggle8 ", m.log.c_str());
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

void test_tuner_opens_and_closes() {
  FakeModel m;
  Ui ui(m);
  tap(ui, 80, kBarY);
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
    tap(ui, 80, kBarY);
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
  RUN_TEST(test_tuner_opens_and_closes);
  RUN_TEST(test_long_names_fit);
  RUN_TEST(test_blocks_header_matches_blocks_json);
  RUN_TEST(test_write_pictures);
  return UNITY_END();
}
