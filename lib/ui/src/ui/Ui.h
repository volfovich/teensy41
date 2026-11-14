// Ui.h - the touch screen's screens: home (preset, chain, meters), preset list, a block's
// settings, the list of models or IRs, the speed trainer, tuner and the save / discard question. Portable: it sees the unit only through UiModel, takes touch
// points, and paints changed parts of the screen into strips (Canvas.h), so the firmware
// (src/Screen.cpp) and the host tests use the same code.
//
// How a screen is written: one layout function per screen names every part with its
// rectangle. The same function runs in three ways: to note what each part would show now
// (a number per part; a part is repainted when its number changes), to paint the parts
// that fall into a strip, and to find the part under a finger.
//
// Threading: everything here runs in loop().
#pragma once

#include <stdint.h>

#include <ui/BlocksGen.h>
#include <ui/Canvas.h>

namespace ui_tft {

struct TunerView {
  bool valid = false;  // a pitch is heard
  int note = 0;        // 0 = C .. 11 = B
  int octave = 0;
  float cents = 0.0f;  // -50 .. +50
  float hz = 0.0f;
};

// One setting of a block as its editor shows it.
struct SettingView {
  char label[24];        // "Mix"
  char text[24];         // the value: "30%", "hall", "ON"
  float fraction = 0.0f; // where the value is in its range, 0 .. 1
  bool toggle = false;   // an on / off setting
  bool on = false;       // a toggle's value
  bool bar = false;      // a number with a range worth showing (not a toggle or a choice)
};

// What the screen needs from the unit. Preset indices are places in the list, 0-based.
struct UiModel {
  virtual ~UiModel() {}
  virtual int presetCount() = 0;
  virtual int presetCurrent() = 0;               // the one that plays; -1 = none
  virtual int presetSaved() = 0;                 // the one recalled before previews began
  virtual const char* presetName(int idx) = 0;   // "03 Lead"
  virtual int presetNumber(int idx) = 0;         // 3, or 0 if the name has no number
  virtual bool presetEdited() = 0;
  virtual bool recall(int idx) = 0;
  virtual bool preview(int idx) = 0;
  virtual void cancelPreview() = 0;
  virtual void keepPreview() = 0;
  virtual bool save() = 0;
  virtual bool blockOn(int block) = 0;           // block: index into kBlocks
  virtual void toggleBlock(int block) = 0;
  // Setting i of a block (i: index into kBlocks[block].settings). False if it does not
  // apply now (a dial of another mode or type) and is not shown.
  virtual bool setting(int block, int i, SettingView* v) = 0;
  // Moves it by a number of its own steps (clamped to its range); a toggle goes on (> 0) or off.
  virtual void settingStep(int block, int i, int steps) = 0;
  // The files a block can load (kind: kFileModel or kFileIr), by place in the unit's list.
  virtual int fileCount(FileKind kind) = 0;
  virtual int fileCurrent(FileKind kind) = 0;              // -1 = none loaded
  virtual const char* fileName(FileKind kind, int idx) = 0;  // without folder and extension
  virtual bool fileLoad(FileKind kind, int idx) = 0;
  virtual int drumTempoNow() = 0;                // BPM as it plays (the trainer raises it)
  virtual float inputDb() = 0;                   // peaks since the last call
  virtual float outputDb() = 0;
  virtual int cpuPercent() = 0;
  virtual void tunerOpen(bool on) = 0;
  virtual TunerView tuner() = 0;
};

class Ui {
 public:
  enum Screen { kHome, kPresets, kTuner, kConfirm, kBlockEdit, kFiles, kTrainer };
  static const int kRowsPerPage = 5;   // the preset list
  static const int kSettingsPerPage = 4;

  explicit Ui(UiModel& model) : m_(model) {}

  // The panel's state in screen coordinates, as often as it is read (also while up).
  void touch(bool down, int x, int y, uint32_t ms);
  // Call every pass: held-button repeats and the meters' timing.
  void tick(uint32_t ms);
  // Paints the next changed strip into buf (room for bufPixels pixels, at least 320 * 8).
  // Returns false when the glass is up to date; else *out is where the strip goes.
  bool render(uint16_t* buf, int bufPixels, Rect* out);
  // Everything is painted again (after the display was reset).
  void invalidate() { drawnCount_ = -1; }

  Screen screen() const { return screen_; }
  int editedBlock() const { return editBlock_; }

 private:
  enum Action { kNone, kPrev, kNext, kOpenList, kBlock, kOpenTuner, kCloseTuner, kRow, kPageUp, kPageDown,
                kListBack, kListOk, kSave, kDiscard, kCancel, kSaveNow, kEditBack, kEditOn, kMinus, kPlus, kFlip,
                kEditUp, kEditDown, kFilePrev, kFileNext, kOpenFiles, kFileRow, kFilesBack, kFilesUp, kFilesDown, kOpenTrainer,
                kTrainerOn, kPlay };
  enum Mode { kMeasure, kPaint, kHit };
  struct Pass {
    Mode mode;
    Canvas* canvas = nullptr;      // kPaint
    int count = 0;                 // kMeasure: parts so far
    int hx = 0, hy = 0;            // kHit: the point
    Action action = kNone;         // kHit: what is there
    int arg = 0;
    Rect rect = {0, 0, 0, 0};
  };
  static const int kMaxParts = 64;
  struct Part {
    Rect r;
    uint32_t sig;
  };

  // A part of the screen: true if it must be painted now.
  bool part(Pass& p, Rect r, uint32_t sig);
  void target(Pass& p, Rect r, Action a, int arg = 0);
  bool pressed(Action a, int arg = 0) const { return held_ && inside_ && heldAction_ == a && heldArg_ == arg; }
  void button(Pass& p, Rect r, const char* label, Action a, int arg = 0, uint16_t colour = 0, bool enabled = true);
  void arrowButton(Pass& p, Rect r, int dir, Action a);
  void meter(Pass& p, int x, int y, int labelW, const char* label, float db);

  void layout(Pass& p);
  void layoutHome(Pass& p);
  void layoutPresets(Pass& p);
  void layoutTuner(Pass& p);
  void layoutConfirm(Pass& p);
  void layoutBlock(Pass& p);
  void layoutFiles(Pass& p);
  void layoutTrainer(Pass& p);
  // One setting with minus and plus in row `row` of the settings area.
  void settingRow(Pass& p, int row, int block, int i, const SettingView& v);
  // The place of a setting in a block by its name in blocks.json; -1 if it has none.
  static int findSetting(int block, const char* ref);
  void fileStep(int dir);
  // The settings of the edited block that are shown now; returns how many.
  int shownSettings(int* idx);

  void fire(Action a, int arg);
  void step(int dir);
  void go(Screen s);
  void ask(bool keepPreview, int target);

  UiModel& m_;
  Screen screen_ = kHome, back_ = kHome;
  // What is on the glass.
  Part drawn_[kMaxParts];
  int drawnCount_ = -1;  // -1: nothing valid
  Screen drawnScreen_ = kHome;
  Part now_[kMaxParts];
  // The strip being painted.
  bool painting_ = false;
  Rect dirty_ = {0, 0, 0, 0};
  int dirtyY_ = 0;
  // Touch.
  bool held_ = false, inside_ = false, repeated_ = false;
  Action heldAction_ = kNone;
  int heldArg_ = 0;
  Rect heldRect_ = {0, 0, 0, 0};
  uint32_t pressMs_ = 0, lastDownMs_ = 0, repeatMs_ = 0;
  // Meters: read 20 times a second, the peak falls slowly.
  uint32_t meterMs_ = 0;
  float inDb_ = -100.0f, outDb_ = -100.0f;
  int cpu_ = 0;
  uint32_t cpuMs_ = 0;
  // Preset list.
  int page_ = 0;
  bool previewing_ = false, editedAtOpen_ = false;
  // A block's settings.
  int editBlock_ = 0, editPage_ = 0, repeats_ = 0, filePage_ = 0;
  // The question.
  bool askKeep_ = false;
  int askTarget_ = -1;
};

}  // namespace ui_tft
