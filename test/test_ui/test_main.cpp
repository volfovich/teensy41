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

#include <dsp/player/TrackIndex.h>
#include <ui/Ui.h>

using namespace ui_tft;

void setUp() {}
void tearDown() {}

namespace {

struct FakeModel : UiModel {
  std::vector<std::string> names = {"01 Recto 91", "02 JCM8OO", "03 Clean Chorus", "04 Lead", "05 Plexi", "06 Tweed", "07 Djent"};
  int current = 1, saved = 1;
  bool edited = false, previewOn = false, editedBefore = false;
  bool on[kBlockCount] = {true, false, true, false, false, true, true, false};
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
  int beat = 0, beats = 4;
  float limDb = 0;
  void drumBeat(int* b, int* n) override {
    *b = on[kBlockCount - 1] ? beat : -1;
    *n = beats;
  }
  float limiterReductionDb() override { return limDb; }
  float inputDb() override { return inDb; }
  float outputDb() override { return outDb; }
  int cpuPercent() override { return cpu; }
  void tunerOpen(bool o) override {
    log += o ? "tunerOn " : "tunerOff ";
    tunerIsOpen = o;
  }
  TunerView tuner() override { return view; }

  // The backing tracks: a small card in the real list (dsp/player/TrackIndex.h). Top:
  // the folders Blues (3 tracks) and Rock (the folder Live with 1 track, and 12 tracks),
  // then 7 tracks.
  std::vector<dsp::TrackIndex::Entry> trackEntries = std::vector<dsp::TrackIndex::Entry>(128);
  std::vector<char> trackPool = std::vector<char>(4096);
  uint16_t trackHits[dsp::TrackBrowser::kMaxHits];
  dsp::TrackIndex index{{trackEntries.data(), 128, trackPool.data(), 4096}};
  dsp::TrackBrowser browser{index, trackHits};
  bool tracksBuilt = buildTracks();
  bool buildTracks() {
    index.begin();
    for (const char* n : {"Blues in A.mp3", "Rock", "Funk_jam_97.mp3", "Slow ballad in E minor with a very long name.wav", "Rock 120.mp3",
                          "Blues", "Shuffle.mp3", "Metal 180.mp3", "Reggae.mp3"})
      index.add(n, !strchr(n, '.'));
    index.endFolder();  // Blues is next
    for (const char* n : {"Slow blues in A.mp3", "Shuffle in E.mp3", "Minor blues 60.wav"}) index.add(n, false);
    index.endFolder();  // Rock
    index.add("Live", true);
    for (int i = 12; i >= 1; i--) {
      char n[32];
      snprintf(n, sizeof(n), "Rock song %02d.mp3", i);
      index.add(n, false);
    }
    index.endFolder();  // Rock/Live
    index.add("Arena rock in D.mp3", false);
    index.endFolder();
    return index.done();
  }
  static std::string shown(const char* name, bool file) {
    std::string n = name;
    if (file) n = n.substr(0, n.rfind('.'));
    for (char& c : n)
      if (c == '_') c = ' ';
    return n;
  }
  TrackView tv;
  int trackOpenId = -1, trackVolHalfDb = -12;
  bool reading = false;  // the card is still being read
  bool trackAny() override { return index.files() > 0; }
  int trackRows() override { return browser.rows(); }
  void trackRow(int i, TrackRow* r) override {
    *r = TrackRow();
    const int id = browser.row(i);
    if (id < 0) return;
    r->isFolder = index.isFolder(id);
    r->open = id == trackOpenId;
    snprintf(r->name, sizeof(r->name), "%s", shown(index.name(id), !r->isFolder).c_str());
    if (browser.searching()) snprintf(r->folder, sizeof(r->folder), "%s", index.parent(id) ? index.name(index.parent(id)) : "TRACKS");
  }
  void trackTitle(char* buf, int n) override {
    if (browser.searching()) snprintf(buf, n, "\"%s\": %d found", browser.text(), browser.rows());
    else snprintf(buf, n, "%s%s", browser.folder() ? index.name(browser.folder()) : "BACKING TRACKS", reading ? " (reading 1234)" : "");
  }
  int trackEnter(int i) override {
    if (browser.row(i) < 0) return 0;
    const int id = browser.enter(i);
    if (id < 0) return 1;
    if (id == trackOpenId) return 2;
    log += "track[" + shown(index.name(id), true) + "] ";
    trackOpenId = id;
    tv = TrackView();
    tv.open = true;
    snprintf(tv.name, sizeof(tv.name), "%s", shown(index.name(id), true).c_str());
    tv.length = 245.0f;
    tv.scanPercent = 40;
    return 2;
  }
  int trackUp() override {
    const int from = browser.searching() ? -1 : browser.folder();
    if (!browser.up()) return -1;
    const int row = browser.rowOf(from);
    return row > 0 ? row : 0;
  }
  void trackFind(const char* text) override {
    log += std::string("find[") + text + "] ";
    browser.find(text);
  }
  int trackListHome() override {
    const int row = trackOpenId >= 0 ? browser.show(trackOpenId) : 0;
    return row > 0 ? row : 0;
  }
  void track(TrackView* v) override { *v = tv; }
  void trackPlay(bool o) override {
    log += o ? "trackPlay " : "trackPause ";
    tv.playing = o;
  }
  void trackStop() override {
    log += "trackStop ";
    tv.playing = false;
    tv.position = tv.looping ? tv.loopFrom : 0.0f;
  }
  std::vector<float> seeks;
  void trackSeek(float seconds) override {
    seeks.push_back(seconds);
    tv.position = seconds;
  }
  void trackLoopMark(bool end) override {
    log += end ? "markB " : "markA ";
    if (!end) {
      tv.loopFrom = tv.position;
      tv.markA = !tv.loopSet;
    } else if (tv.markA || tv.loopSet) {
      tv.loopTo = tv.position;
      tv.loopSet = tv.looping = tv.loopTo > tv.loopFrom;
      tv.markA = false;
    }
  }
  // The loop in bars: four beats to a bar, the first beat at 0.
  void trackLoopBars(LoopBarsView* v) override {
    *v = LoopBarsView();
    v->set = tv.loopSet && tv.bpm > 0.0f;
    v->looping = tv.looping;
    if (!v->set) return;
    const float beat = 60.0f / tv.bpm;
    const int first = (int)lrintf(tv.loopFrom / beat);
    v->bar = first / 4 + 1;
    v->beat = first % 4 + 1;
    v->bars = (int)lrintf((tv.loopTo - tv.loopFrom) / beat / 4.0f);
    if (v->bars < 1) v->bars = 1;
    v->from = tv.loopFrom;
    v->to = tv.loopTo;
  }
  bool trackLoopBarsSet(int bar, int beat, int bars) override {
    if (tv.bpm <= 0.0f || bar < 1 || beat < 1 || beat > 4 || bars < 1) return false;
    const float len = 60.0f / tv.bpm, from = (float)((bar - 1) * 4 + beat - 1) * len;
    if (from >= tv.length) return false;
    log += "loop[" + std::to_string(bar) + "." + std::to_string(beat) + " x" + std::to_string(bars) + "] ";
    tv.loopFrom = from;
    tv.loopTo = from + (float)(bars * 4) * len;
    if (tv.loopTo > tv.length) tv.loopTo = tv.length;
    tv.loopSet = tv.looping = true;
    tv.markA = false;
    return true;
  }
  void trackLoopSwitch() override {
    if (!tv.loopSet) return;
    tv.looping = !tv.looping;
    log += tv.looping ? "loopOn " : "loopOff ";
  }
  void trackTempoStep(int percent) override {
    tv.tempoPercent += percent;
    tv.tempoPercent = tv.tempoPercent < 50 ? 50 : (tv.tempoPercent > 150 ? 150 : tv.tempoPercent);
  }
  void trackBpmScale(bool twice) override {
    log += twice ? "double " : "half ";
    tv.bpm = twice ? tv.bpm * 2 : tv.bpm / 2;
  }
  void trackVolume(SettingView* v) override {
    *v = SettingView();
    snprintf(v->label, sizeof(v->label), "Volume");
    snprintf(v->text, sizeof(v->text), "%.1f dB", trackVolHalfDb / 2.0);
    v->fraction = (trackVolHalfDb + 120) / 132.0f;
    v->bar = true;
  }
  void trackVolumeStep(int steps) override {
    trackVolHalfDb += steps;
    trackVolHalfDb = trackVolHalfDb < -120 ? -120 : (trackVolHalfDb > 12 ? 12 : trackVolHalfDb);
  }
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
const int kPrevX = 22, kNextX = 298, kArrowY = 44, kNameX = 160, kNameY = 44;  // the preset's bar
// The blocks on the home screen: the chain's six tiles in one lane, then the limiter in
// the bottom line (6), the drums' play button (7) and their tempo, which opens the trainer (8).
const int kPlaysX = 246, kDrumsY = 170;  // what the drums play: opens the trainer's second page
int tileX(int i) { return i < 6 ? 4 + i * 52 + 24 : (i == 6 ? 217 : (i == 7 ? 34 : 120)); }
int tileY(int i) { return i < 6 ? 112 : (i == 6 ? 222 : kDrumsY); }
int rowY(int row) { return 24 + row * 36 + 17; }
const int kTunerX = 33, kSaveBtnX = 93, kTrackX = 155, kVolX = 282;  // the home screen's bottom line (with the limiter between)
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
  TEST_ASSERT_EQUAL_INT(49 * 52, g.pixels);
  TEST_ASSERT_EQUAL_INT(4 + 3 * 52, g.x0);
  TEST_ASSERT_EQUAL_INT(86, g.y0);

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
  tap(ui, tileX(7), tileY(7));
  TEST_ASSERT_EQUAL_STRING("toggle1 toggle7 ", m.log.c_str());
  m.log.clear();
  tap(ui, 160, 138);  // between the chain and the drums
  tap(ui, 54, 112);   // between two tiles
  tap(ui, 80, 76);    // the loaded files' line
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
  TEST_ASSERT_EQUAL_INT(36 * 44, g.pixels);
  for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms);
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels > 36 * 44);  // the button and what the new preset changed
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
  tap(ui, kNameX, kNameY);
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
  const int kDelay = 4;
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
  TEST_ASSERT_EQUAL_STRING("toggle4 ", m.log.c_str());
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
  const int kDelay = 4;
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
  tap(ui, tileX(8), tileY(8));
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

// The home screen's drums panel and the limiter beside the master volume.
void test_home_drums_and_limiter() {
  FakeModel m;
  Ui ui(m);
  const int kLimiter = 6, kDrums = 7;
  TEST_ASSERT_EQUAL_STRING("limiter", kBlocks[kLimiter].id);
  TEST_ASSERT_EQUAL_STRING("drums", kBlocks[kDrums].id);

  // PLAY starts and stops the drums; holding it opens the block's settings.
  tap(ui, tileX(kDrums), tileY(kDrums));
  TEST_ASSERT_TRUE(m.on[kDrums]);
  tap(ui, tileX(kDrums), tileY(kDrums));
  TEST_ASSERT_FALSE(m.on[kDrums]);
  hold(ui, tileX(kDrums), tileY(kDrums), 700);
  TEST_ASSERT_EQUAL_INT(Ui::kBlockEdit, ui.screen());
  TEST_ASSERT_EQUAL_INT(kDrums, ui.editedBlock());
  tap(ui, kEditBackX, kEditTopY);

  // The tempo opens the trainer; what plays opens its second page (style first).
  m.log.clear();
  tap(ui, tileX(8), tileY(8));
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(51, m.val[kDrums][FakeModel::find(kDrums, "bpm")]);
  tap(ui, kEditBackX, kEditTopY);
  tap(ui, kPlaysX, kDrumsY);
  TEST_ASSERT_EQUAL_INT(Ui::kTrainer, ui.screen());
  tap(ui, kPlusX, setY(0));
  TEST_ASSERT_EQUAL_INT(2, m.val[kDrums][FakeModel::find(kDrums, "drum style")]);
  tap(ui, kEditBackX, kEditTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());

  // The limiter is a block like the tiles: a tap switches it, holding opens its settings.
  m.log.clear();
  tap(ui, tileX(kLimiter), tileY(kLimiter));
  TEST_ASSERT_EQUAL_STRING("toggle6 ", m.log.c_str());
  hold(ui, tileX(kLimiter), tileY(kLimiter), 700);
  TEST_ASSERT_EQUAL_INT(Ui::kBlockEdit, ui.screen());
  TEST_ASSERT_EQUAL_INT(kLimiter, ui.editedBlock());
  tap(ui, kEditBackX, kEditTopY);
  tap(ui, tileX(kLimiter), tileY(kLimiter));
  TEST_ASSERT_TRUE(m.on[kLimiter]);

  // A beat repaints the beat dots alone; the limiter at work repaints its button alone.
  m.on[kDrums] = true;
  ui.tick(g_ms += 1000);
  Glass g;
  g.paint(ui);
  m.beat = 1;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(124 * 16, g.pixels);
  m.limDb = 6;
  ui.tick(g_ms += 60);
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(54 * 36, g.pixels);
  m.limDb = 0.4f;  // too little to show: the bar falls back by itself
  for (int i = 0; i < 20; i++) ui.tick(g_ms += 60);
  g.paint(ui);
  Ui fresh(m);
  fresh.tick(g_ms + 1000);
  Glass g2;
  g2.paint(fresh);
  TEST_ASSERT_TRUE(g.px == g2.px);

  // With no preset recalled, a song chosen and a bar of seven beats everything still paints.
  m.current = -1;
  m.val[kDrums][FakeModel::find(kDrums, "drum song")] = 3;
  m.beats = 7;
  m.beat = 6;
  g.paint(ui);
  for (uint16_t v : g.px) TEST_ASSERT_TRUE(v != 0xF81F);
}

// Every screen, used at random: whatever is touched or changes in the unit, the glass must
// show exactly what painting the whole screen anew would show. This finds a part that
// paints outside its rectangle (it is cut off when it alone is repainted) and a part
// that does not notice a change of what it shows.
void test_random_use_keeps_the_glass_true() {
  FakeModel m;
  Ui ui(m);
  Glass g;
  g.paint(ui);
  uint32_t seed = 20261114u;
  auto rnd = [&seed](int n) {
    seed = seed * 1664525u + 1013904223u;
    return (int)((seed >> 10) % (uint32_t)n);
  };
  int seen[16] = {0};
  auto check = [&](int step, const char* what) {
    ui.tick(g_ms += 60);
    g.paint(ui);
    seen[ui.screen()]++;
    ui.invalidate();
    Glass whole;
    whole.paint(ui);
    for (int i = 0; i < kScreenW * kScreenH; i++)
      if (g.px[i] != whole.px[i]) {
        g.save("wrong_glass");
        whole.save("wrong_whole");
        char msg[160];
        snprintf(msg, sizeof(msg), "step %d (%s), screen %d: the glass differs from a whole repaint at x %d y %d (see .pio/ui/wrong_*)",
                 step, what, (int)ui.screen(), i % kScreenW, i / kScreenW);
        TEST_FAIL_MESSAGE(msg);
      }
  };
  for (int step = 0; step < 6000; step++) {
    const int x = rnd(kScreenW), y = rnd(kScreenH), kind = rnd(12);
    if (kind < 5) {
      // A tap, looked at while the finger is down and after it.
      for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(true, x, y, g_ms), ui.tick(g_ms);
      check(step, "finger down");
      for (int i = 0; i < 8; i++, g_ms += 10) ui.touch(false, 0, 0, g_ms), ui.tick(g_ms);
      check(step, "tap");
    } else if (kind == 5) {
      hold(ui, x, y, 700 + rnd(900));
      check(step, "hold");
    } else if (kind == 6) {
      drag(ui, x, y, rnd(kScreenW), y);
      check(step, "drag");
    } else if (kind == 7) {
      // The way home, so that every screen is opened many times.
      tap(ui, 36, 16);
      tap(ui, 40, 222);
      // And now and then on to a screen that random taps seldom find: the track's loop in bars.
      if (!rnd(4)) {
        tap(ui, 155, 222);  // TRACK on home
        tap(ui, 250, 41);   // the bar and beat readout
      }
      check(step, "back");
    } else {
      // The unit changes by itself: the drums' beat and tempo, levels, the limiter, a block
      // switched or a setting changed from the app, the tuner's reading.
      m.beat = rnd(m.beats);
      if (!rnd(6)) m.beats = 1 + rnd(12);
      if (!rnd(4)) m.tempoNow = 40 + rnd(200);
      if (!rnd(4)) m.on[rnd(kBlockCount)] ^= true;
      if (!rnd(4)) m.inDb = (float)-rnd(70), m.outDb = (float)-rnd(70);
      if (!rnd(4)) m.limDb = (float)rnd(14);
      if (!rnd(4)) m.cpu = rnd(100);
      if (!rnd(6)) m.edited = !m.edited;
      if (!rnd(6)) m.masterHalfDb = -rnd(121);
      if (!rnd(6)) {
        const int b = rnd(kBlockCount), i = rnd(kBlocks[b].settingCount);
        m.settingStep(b, i, rnd(3) - 1);
      }
      if (!rnd(6)) m.model = rnd((int)m.models.size()), m.ir = rnd((int)m.irs.size());
      // A track is open (from the shell, say), its tempo is found or not, and it moves on.
      if (!rnd(5)) m.tv.open = true, m.tv.length = 245.0f, m.tv.bpm = rnd(3) ? 60.0f + (float)rnd(120) : 0.0f, m.tv.scanPercent = 100;
      if (!rnd(4)) m.tv.bar = rnd(60), m.tv.beat = 1 + rnd(4), m.tv.position = (float)rnd(240);
      m.view.valid = rnd(2);
      m.view.note = rnd(12);
      m.view.cents = (float)(rnd(100) - 50);
      m.view.hz = 80.0f + (float)rnd(400);
      check(step, "the unit changed");
    }
  }
  for (int s = Ui::kHome; s <= Ui::kLoopBars; s++) {
    char msg[64];
    snprintf(msg, sizeof(msg), "screen %d was never looked at", s);
    TEST_ASSERT_TRUE_MESSAGE(seen[s] > 0, msg);
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
  tap(ui, tileX(8), tileY(8));
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
  const int kDelay = 4;
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
  hold(ui, tileX(1), tileY(1), 700);  // the gate
  tap(ui, kPlusX, setY(1));
  TEST_ASSERT_EQUAL_INT(51, m.val[1][1]);
}

void test_speed_trainer_is_one_tap_from_home() {
  FakeModel m;
  Ui ui(m);
  const int kDrums = kBlockCount - 1, kTrainX = tileX(8), kTrainY = tileY(8);
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
  TEST_ASSERT_EQUAL_STRING("toggle7 ", m.log.c_str());

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
  TEST_ASSERT_EQUAL_INT(104 * 48, g.pixels);  // the tempo
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

// The tracks screen's places (see Ui.cpp's layoutTracks).
const int kTrackNameX = 190, kTrackTopY = 16, kSeekY = 64, kTransportY = 100, kTempoY = 144, kBpmY = 185, kTrackVolY = 222;
const int kTrackPlayX = 46, kTrackStopX = 123, kMarkAX = 181, kMarkBX = 223, kLoopX = 281;

void test_the_backing_track_screen() {
  FakeModel m;
  Ui ui(m);
  Glass g;
  g.paint(ui);
  // One tap from home; without a track only the name's button and BACK do anything.
  tap(ui, kTrackX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  tap(ui, kTrackPlayX, kTransportY);
  tap(ui, kMarkAX, kTransportY);
  tap(ui, 160, kSeekY);
  tap(ui, 246, kBpmY);
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  TEST_ASSERT_TRUE(m.seeks.empty());
  // The name opens the list of tracks; a tap opens one and comes back.
  tap(ui, kTrackNameX, kTrackTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  tap(ui, kDownX, kBarY);  // the list's page down: the second page
  tap(ui, 160, rowY(1));   // its second row (two folders, then the tracks by name)
  TEST_ASSERT_EQUAL_STRING("track[Rock 120] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  // BACK from the list leaves the track as it is.
  tap(ui, kTrackNameX, kTrackTopY);
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  TEST_ASSERT_EQUAL_STRING("track[Rock 120] ", m.log.c_str());
  // Play and pause are one button; stop is its own.
  m.log.clear();
  tap(ui, kTrackPlayX, kTransportY);
  tap(ui, kTrackPlayX, kTransportY);
  tap(ui, kTrackPlayX, kTransportY);
  tap(ui, kTrackStopX, kTransportY);
  TEST_ASSERT_EQUAL_STRING("trackPlay trackPause trackPlay trackStop ", m.log.c_str());
  // A tap on the bar goes there: 300 px for 245 s, from x 10.
  tap(ui, 160, kSeekY);
  TEST_ASSERT_EQUAL_INT(1, (int)m.seeks.size());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 122.5f, m.seeks[0]);
  // A slide moves from where the track is by the finger's way (the first 10 px begin it),
  // a few times a second, and ends where the finger left: 100 px = 81.7 s further.
  m.seeks.clear();
  drag(ui, 60, kSeekY, 170, kSeekY);
  TEST_ASSERT_TRUE(m.seeks.size() >= 2 && m.seeks.size() <= 5);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 122.5f + 81.7f, m.seeks.back());
  // The loop: A, then B further on; LOOP ends it.
  m.log.clear();
  m.tv.position = 30.0f;
  tap(ui, kMarkAX, kTransportY);
  tap(ui, kLoopX, kTransportY);  // no loop yet: the button does nothing
  m.tv.position = 45.0f;
  tap(ui, kMarkBX, kTransportY);
  TEST_ASSERT_TRUE(m.tv.looping);
  // LOOP switches it off and on again; its points stay where they were set.
  tap(ui, kLoopX, kTransportY);
  TEST_ASSERT_FALSE(m.tv.looping);
  TEST_ASSERT_TRUE(m.tv.loopSet);
  tap(ui, kLoopX, kTransportY);
  TEST_ASSERT_TRUE(m.tv.looping);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, m.tv.loopFrom);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, m.tv.loopTo);
  tap(ui, kLoopX, kTransportY);
  TEST_ASSERT_EQUAL_STRING("markA markB loopOff loopOn loopOff ", m.log.c_str());
  // The tempo: a step a tap, faster while held.
  tap(ui, kMinusX, kTempoY);
  TEST_ASSERT_EQUAL_INT(99, m.tv.tempoPercent);
  hold(ui, kMinusX, kTempoY, 2500);
  TEST_ASSERT_TRUE(m.tv.tempoPercent < 70 && m.tv.tempoPercent >= 50);
  hold(ui, kPlusX, kTempoY, 6000);
  TEST_ASSERT_EQUAL_INT(150, m.tv.tempoPercent);
  // The track's own tempo: nothing to correct until it is known.
  m.log.clear();
  tap(ui, 246, kBpmY);
  tap(ui, 294, kBpmY);
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  m.tv.scanPercent = 100;
  m.tv.bpm = 97.0f;
  tap(ui, 246, kBpmY);
  tap(ui, 294, kBpmY);
  TEST_ASSERT_EQUAL_STRING("half double ", m.log.c_str());
  // Its volume.
  tap(ui, kPlusX, kTrackVolY);
  tap(ui, kPlusX, kTrackVolY);
  tap(ui, kMinusX, kTrackVolY);
  TEST_ASSERT_EQUAL_INT(-11, m.trackVolHalfDb);
  // Only what changed is repainted: the time and the bar as the track moves on.
  g.paint(ui);
  m.tv.position += 1.0f;
  g.paint(ui);
  TEST_ASSERT_TRUE(g.pixels > 0 && g.pixels <= 120 * 22 + 304 * 22);
  // Home shows the track playing.
  tap(ui, 36, kTrackTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  g.paint(ui);
  m.tv.playing = true;
  g.paint(ui);
  TEST_ASSERT_EQUAL_INT(62 * 36, g.pixels);
}

// The backing tracks' list: folders, paging, the search.
void test_the_track_list_folders_and_find() {
  FakeModel m;
  Ui ui(m);
  Glass g;
  TEST_ASSERT_TRUE(m.tracksBuilt);
  tap(ui, kTrackX, kBarY);
  tap(ui, kTrackNameX, kTrackTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  // The top: the two folders first. Into Rock: its folder Live, then 12 tracks, three pages.
  tap(ui, 160, rowY(1));
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  TEST_ASSERT_EQUAL_STRING("Rock", m.index.name(m.browser.folder()));
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  // Holding the page button goes on paging (after 0.6 s, every 0.3 s) and stops at the end.
  hold(ui, kDownX, kBarY, 1300);
  tap(ui, 160, rowY(2));  // the last page's third row: rows 10, 11, 12
  TEST_ASSERT_EQUAL_STRING("track[Rock song 12] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  // The list opens again where the open track is: Rock, its last page.
  tap(ui, kTrackNameX, kTrackTopY);
  TEST_ASSERT_EQUAL_STRING("Rock", m.index.name(m.browser.folder()));
  tap(ui, 160, rowY(1));
  TEST_ASSERT_EQUAL_STRING("track[Rock song 12] track[Rock song 11] ", m.log.c_str());
  // A tap on the open track opens nothing: just back.
  tap(ui, kTrackNameX, kTrackTopY);
  tap(ui, 160, rowY(1));
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  TEST_ASSERT_EQUAL_STRING("track[Rock song 12] track[Rock song 11] ", m.log.c_str());
  // Holding page up comes back to the first page; into Live and its one track.
  tap(ui, kTrackNameX, kTrackTopY);
  hold(ui, kUpX, kBarY, 1300);
  tap(ui, 160, rowY(0));
  TEST_ASSERT_EQUAL_STRING("Live", m.index.name(m.browser.folder()));
  tap(ui, 160, rowY(3));  // an empty row
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  // BACK goes up a folder at a time (to the page the folder is on), and from the top to the track's screen.
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_STRING("Rock", m.index.name(m.browser.folder()));
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_INT(0, m.browser.folder());
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());

  // FIND: the keyboard, in lower case; nothing typed finds nothing; CANCEL comes back to the list.
  m.log.clear();
  tap(ui, kTrackNameX, kTrackTopY);
  tap(ui, kOkX, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  tap(ui, 239, kBarY);  // FIND with nothing typed
  TEST_ASSERT_EQUAL_INT(Ui::kName, ui.screen());
  tap(ui, 80, kBarY);
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, kOkX, kBarY);
  type(ui, "blues in");
  tap(ui, 239, kBarY);
  TEST_ASSERT_EQUAL_STRING("find[blues in] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  // The hits, from all folders: "Blues in A" at the top, then the three in Blues (the
  // folder's name counts: "Minor blues 60", "Shuffle in E", "Slow blues in A"). A tap opens one.
  TEST_ASSERT_EQUAL_INT(4, m.browser.rows());
  g.paint(ui);
  tap(ui, 160, rowY(3));
  TEST_ASSERT_EQUAL_STRING("find[blues in] track[Slow blues in A] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  // The list then shows where that track is.
  tap(ui, kTrackNameX, kTrackTopY);
  TEST_ASSERT_FALSE(m.browser.searching());
  TEST_ASSERT_EQUAL_STRING("Blues", m.index.name(m.browser.folder()));
  // Nothing found: said so, nothing to tap, BACK returns to the folder.
  tap(ui, kOkX, kBarY);
  type(ui, "zz");
  tap(ui, 239, kBarY);
  TEST_ASSERT_EQUAL_INT(0, m.browser.rows());
  g.paint(ui);
  tap(ui, 160, rowY(0));
  TEST_ASSERT_EQUAL_INT(Ui::kFiles, ui.screen());
  tap(ui, kBackX, kBarY);
  TEST_ASSERT_FALSE(m.browser.searching());
  TEST_ASSERT_EQUAL_STRING("Blues", m.index.name(m.browser.folder()));
  // The keyboard still saves presets as before after it was used to find.
  tap(ui, kBackX, kBarY);
  tap(ui, kBackX, kBarY);
  tap(ui, 36, kTrackTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
  m.log.clear();
  tap(ui, kSaveBtnX, kBarY);
  tap(ui, kMenuX, kNewY);
  type(ui, "ab");
  tap(ui, 239, kBarY);
  TEST_ASSERT_EQUAL_STRING("saveAs[Ab] ", m.log.c_str());
  TEST_ASSERT_EQUAL_INT(Ui::kHome, ui.screen());
}

// The backing track's loop set in bars.
void test_the_loop_set_in_bars() {
  FakeModel m;
  Ui ui(m);
  Glass g;
  const int kReadoutX = 250, kReadoutY = 41, kStartY = 71, kLenY = 135, kLoopRowY = 218;
  const int kBarBackX = 27, kBeatBackX = 77, kBeatOnX = 243, kBarOnX = 293, kHereX = 282;
  tap(ui, kTrackX, kBarY);
  tap(ui, kTrackNameX, kTrackTopY);
  tap(ui, 160, rowY(2));  // a track; its tempo is not known yet
  m.log.clear();
  // Without the tempo there are no bars: the readout opens nothing.
  tap(ui, kReadoutX, kReadoutY);
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  // 120 BPM: a beat is 0.5 s, a bar 2 s. The track is in bar 6 (10 s to 12 s).
  m.tv.scanPercent = 100;
  m.tv.bpm = 120.0f;
  m.tv.position = 11.2f;
  m.tv.bar = 6;
  m.tv.beat = 3;
  tap(ui, kReadoutX, kReadoutY);
  TEST_ASSERT_EQUAL_INT(Ui::kLoopBars, ui.screen());
  g.paint(ui);
  // Nothing is set by opening it. LOOP makes what is shown the loop: 4 bars from bar 6.
  TEST_ASSERT_EQUAL_STRING("", m.log.c_str());
  tap(ui, 80, kLoopRowY);
  TEST_ASSERT_EQUAL_STRING("loop[6.1 x4] ", m.log.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, m.tv.loopFrom);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, m.tv.loopTo);
  // The start: a beat on, a beat back over the bar's line, a bar on, a bar back.
  m.log.clear();
  tap(ui, kBeatOnX, kStartY);
  tap(ui, kBeatBackX, kStartY);
  tap(ui, kBeatBackX, kStartY);
  tap(ui, kBarOnX, kStartY);
  tap(ui, kBarBackX, kStartY);
  TEST_ASSERT_EQUAL_STRING("loop[6.2 x4] loop[6.1 x4] loop[5.4 x4] loop[6.4 x4] loop[5.4 x4] ", m.log.c_str());
  // The length jumps through 1, 2, 4, 8, 16, 32 and stops at both ends.
  m.log.clear();
  tap(ui, kBeatOnX, kLenY);
  tap(ui, kBeatOnX, kLenY);
  tap(ui, kBeatOnX, kLenY);
  tap(ui, kBeatOnX, kLenY);  // 32 is the longest
  for (int i = 0; i < 6; i++) tap(ui, kBeatBackX, kLenY);  // 1 is the shortest
  TEST_ASSERT_EQUAL_STRING("loop[5.4 x8] loop[5.4 x16] loop[5.4 x32] loop[5.4 x16] loop[5.4 x8] loop[5.4 x4] loop[5.4 x2] loop[5.4 x1] ",
                           m.log.c_str());
  // Holding a start button goes on stepping. Before bar 1 there is nothing: it stops at 1.1.
  m.log.clear();
  hold(ui, kBarBackX, kStartY, 2600);
  TEST_ASSERT_EQUAL_STRING("loop[4.4 x1] loop[3.4 x1] loop[2.4 x1] loop[1.4 x1] ", m.log.c_str());
  for (int i = 0; i < 5; i++) tap(ui, kBeatBackX, kStartY);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, m.tv.loopFrom);
  // Past the track's end there is nothing either (245 s: bar 123 starts at 244 s).
  m.log.clear();
  m.tv.position = 243.0f;
  m.tv.bar = 122;
  tap(ui, kHereX, kTrackTopY);
  tap(ui, kBarOnX, kStartY);
  tap(ui, kBarOnX, kStartY);
  TEST_ASSERT_EQUAL_STRING("loop[122.1 x1] loop[123.1 x1] ", m.log.c_str());
  // HERE: the bar the track is in, the length as it is. LOOP switches the loop; PLAY plays.
  m.log.clear();
  m.tv.position = 67.0f;
  m.tv.bar = 34;
  tap(ui, kHereX, kTrackTopY);
  tap(ui, 80, kLoopRowY);
  TEST_ASSERT_FALSE(m.tv.looping);
  TEST_ASSERT_TRUE(m.tv.loopSet);
  tap(ui, 80, kLoopRowY);
  tap(ui, 240, kLoopRowY);
  TEST_ASSERT_EQUAL_STRING("loop[34.1 x1] loopOff loopOn trackPlay ", m.log.c_str());
  // BACK returns to the track's screen, where the loop shows; opened again (also by holding
  // LOOP there), the screen shows the loop as it is.
  tap(ui, 36, kTrackTopY);
  TEST_ASSERT_EQUAL_INT(Ui::kTracks, ui.screen());
  hold(ui, kLoopX, kTransportY, 700);
  TEST_ASSERT_EQUAL_INT(Ui::kLoopBars, ui.screen());
  TEST_ASSERT_TRUE(m.tv.looping);  // the hold did not switch it
  m.log.clear();
  tap(ui, kBeatOnX, kLenY);
  TEST_ASSERT_EQUAL_STRING("loop[34.1 x2] ", m.log.c_str());
  // A loop set by ear with a length that is none of the steps: plus and minus go to the next one.
  m.tv.loopTo = m.tv.loopFrom + 6.0f;  // 3 bars
  tap(ui, 36, kTrackTopY);
  tap(ui, kReadoutX, kReadoutY);
  m.log.clear();
  tap(ui, kBeatOnX, kLenY);
  tap(ui, kBeatBackX, kLenY);
  tap(ui, kBeatBackX, kLenY);
  TEST_ASSERT_EQUAL_STRING("loop[34.1 x4] loop[34.1 x2] loop[34.1 x1] ", m.log.c_str());
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
  // A long preset name (24 characters is the most) fits beside its number on the home
  // screen in the small font without being cut: 164 px are left of the bar's 232.
  TEST_ASSERT_TRUE(Canvas::textWidth(kFontSmall, "WWWWWWWWWWWWWWWWWWWWWWWW") > 164);  // only such a one would not
  TEST_ASSERT_TRUE(Canvas::textWidth(kFontSmall, "Preset With A Long Name!") <= 164);
  // The huge font has the digits and note names only.
  TEST_ASSERT_NOT_NULL(kFontHuge.glyph('7'));
  TEST_ASSERT_NOT_NULL(kFontHuge.glyph('#'));
  TEST_ASSERT_NULL(kFontHuge.glyph('x'));
}

void test_blocks_header_matches_blocks_json() {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, system("python3 tools/make_blocks_header.py --check > /dev/null"),
                                "lib/ui/src/ui/BlocksGen.h is out of date: run tools/make_blocks_header.py");
  TEST_ASSERT_EQUAL_INT(8, kBlockCount);
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
    m.on[1] = m.on[3] = m.on[4] = m.on[7] = true;
    m.outDb = -0.5f;
    ui.tick(g_ms += 1000);
    ui.touch(true, tileX(3), tileY(3), g_ms);
    g.paint(ui);
    g.save("home_edited");
  }
  {
    // Everything at work: a drum song in 6/8 on its third beat, the limiter turning down.
    FakeModel m;
    const int d = kBlockCount - 1;
    for (bool& on : m.on) on = true;
    m.val[3][FakeModel::find(3, "mod type")] = 1;
    m.val[4][FakeModel::find(4, "dtype")] = 1;
    m.val[5][FakeModel::find(5, "rmode")] = 3;
    m.val[d][FakeModel::find(d, "drum song")] = 2;
    m.tempoNow = 126;
    m.beats = 6;
    m.beat = 2;
    m.limDb = 7;
    m.masterHalfDb = -25;
    m.inDb = -14;
    m.outDb = -4;
    Ui ui(m);
    ui.tick(g_ms += 1000);
    Glass g;
    g.paint(ui);
    g.save("home_playing");
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
    m.on[4] = true;
    m.val[4][1] = 25;
    m.val[4][3] = 35;
    Ui ui(m);
    hold(ui, tileX(4), tileY(4), 700);
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
    tap(ui, tileX(8), tileY(8));
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
  {
    FakeModel m;
    Ui ui(m);
    Glass g;
    tap(ui, kTrackX, kBarY);
    g.paint(ui);
    g.save("tracks_none");
    tap(ui, kTrackNameX, kTrackTopY);
    g.paint(ui);
    g.save("tracks_list");
    tap(ui, 160, rowY(1));  // into Rock
    g.paint(ui);
    g.save("tracks_folder");
    tap(ui, kOkX, kBarY);   // FIND
    type(ui, "blues");
    g.paint(ui);
    g.save("tracks_find");
    tap(ui, 239, kBarY);
    g.paint(ui);
    g.save("tracks_results");
    tap(ui, 160, rowY(2));
    m.tv.position = 12.0f;
    g.paint(ui);
    g.save("tracks_scanning");
    m.tv.scanPercent = 100;
    m.tv.bpm = 97.0f;
    m.tv.playing = true;
    m.tv.position = 83.4f;
    m.tv.loopSet = m.tv.looping = true;
    m.tv.loopFrom = 59.4f;
    m.tv.loopTo = 99.0f;
    m.tv.tempoPercent = 80;
    m.tv.bar = 34;
    m.tv.beat = 3;
    g.paint(ui);
    g.save("tracks_playing");
    tap(ui, 250, 41);  // the bar and beat readout: the loop set in bars
    g.paint(ui);
    g.save("loop_bars");
    tap(ui, 36, kTrackTopY);
    g.paint(ui);
    tap(ui, 36, kTrackTopY);
    g.paint(ui);
    g.save("home_track");
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
  RUN_TEST(test_home_drums_and_limiter);
  RUN_TEST(test_random_use_keeps_the_glass_true);
  RUN_TEST(test_save_over_and_as_new);
  RUN_TEST(test_rename_and_delete);
  RUN_TEST(test_choices_have_a_list);
  RUN_TEST(test_master_volume);
  RUN_TEST(test_dragging_a_value);
  RUN_TEST(test_the_backing_track_screen);
  RUN_TEST(test_the_track_list_folders_and_find);
  RUN_TEST(test_the_loop_set_in_bars);
  RUN_TEST(test_tuner_opens_and_closes);
  RUN_TEST(test_long_names_fit);
  RUN_TEST(test_blocks_header_matches_blocks_json);
  RUN_TEST(test_write_pictures);
  return UNITY_END();
}
