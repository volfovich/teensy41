// Ui.h - the touch screen's screens: home (meters, preset, chain, drums), preset list, a block's
// settings, the list of models or IRs, the speed trainer (with what the drums play), saving (over, or as a new preset with a
// typed name), renaming and deleting the recalled preset, the master volume, the backing track (file, play, where it
// is, loop, tempo, its own tempo and bar 1, volume), tuner and the save / discard question. Portable: it sees the unit only through UiModel, takes touch
// points, and paints changed parts of the screen into strips (Canvas.h), so the firmware
// (src/Screen.cpp) and the host tests use the same code.
//
// A setting chosen by name with more than two choices (the drums' style and song, a type, a
// mode) opens a list of them by a tap on it.
//
// A value with a bar (a block's settings, the trainer's, the master volume) also follows a
// finger dragged sideways along it; a tap on it changes nothing.
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
  int choices = 0;       // a value chosen by name: how many there are (0: not one)
  int choice = 0;        // and which it is now, 0-based
};

// The backing track as its screen shows it.
struct TrackView {
  bool open = false;        // a track is open
  char name[40] = "";       // without folder and extension
  bool playing = false;
  float position = 0.0f, length = 0.0f;  // seconds
  bool looping = false;
  float loopFrom = 0.0f, loopTo = 0.0f;  // seconds
  bool markA = false;       // the loop's start is marked and waits for its end
  int tempoPercent = 100;   // of the track's own tempo
  float bpm = 0.0f;         // the track's own tempo; 0: not known
  bool sure = true;         // false: the scan found no steady beat to rely on
  int scanPercent = 100;    // under 100: the scan for the tempo is still on its way
  int bar = 0, beat = 0;    // where the track is (with a tempo known)
};

// A row of the backing tracks' list: a folder, or a track.
struct TrackRow {
  char name[48] = "";    // a track's without its extension
  char folder[32] = "";  // a search's hit: the folder it is in ("" in a folder's own list)
  bool isFolder = false;
  bool open = false;     // the track that is open
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
  // Saves the sound as a new preset, which becomes the recalled one ("" = "Preset NN").
  virtual bool saveAs(const char* name) = 0;
  // Renames the recalled preset (its number stays; false for an empty or a taken name), and
  // deletes it: the sound keeps playing, with no preset recalled.
  virtual bool rename(const char* name) = 0;
  virtual bool remove() = 0;
  // The master volume (the unit's last gain, for all presets), as a setting.
  virtual void master(SettingView* v) = 0;
  virtual void masterStep(int steps) = 0;
  virtual void masterSet(float fraction) = 0;
  virtual bool blockOn(int block) = 0;           // block: index into kBlocks
  virtual void toggleBlock(int block) = 0;
  // Setting i of a block (i: index into kBlocks[block].settings). False if it does not
  // apply now (a dial of another mode or type) and is not shown.
  virtual bool setting(int block, int i, SettingView* v) = 0;
  // Moves it by a number of its own steps (clamped to its range); a toggle goes on (> 0) or off.
  virtual void settingStep(int block, int i, int steps) = 0;
  // Puts a setting with a bar at a place in its range (0 .. 1), on its nearest step.
  virtual void settingSet(int block, int i, float fraction) = 0;
  // A setting chosen by name: the name of choice idx, and choosing it.
  virtual const char* settingChoice(int block, int i, int idx) = 0;
  virtual void settingPick(int block, int i, int idx) = 0;
  // The files a block can load (kind: kFileModel or kFileIr), by place in the unit's list.
  virtual int fileCount(FileKind kind) = 0;
  virtual int fileCurrent(FileKind kind) = 0;              // -1 = none loaded
  virtual const char* fileName(FileKind kind, int idx) = 0;  // without folder and extension
  virtual bool fileLoad(FileKind kind, int idx) = 0;
  virtual int drumTempoNow() = 0;                // BPM as it plays (the trainer raises it)
  // The beat sounding now in the drums' bar (0-based; -1 while they are stopped) with the
  // bar's beats.
  virtual void drumBeat(int* beat, int* beats) = 0;
  // How far the limiter turned the level down at most since the last call, in dB (0: not at all).
  virtual float limiterReductionDb() = 0;
  virtual float inputDb() = 0;                   // peaks since the last call
  virtual float outputDb() = 0;
  virtual int cpuPercent() = 0;
  virtual void tunerOpen(bool on) = 0;
  virtual TunerView tuner() = 0;
  // The backing tracks (the files in /TRACKS and the folders below it). Their list shows
  // one folder at a time, its folders first, or the hits of a search over all folders.
  virtual bool trackAny() = 0;                   // there are tracks on the card
  virtual int trackRows() = 0;
  virtual void trackRow(int i, TrackRow* r) = 0;
  virtual void trackTitle(char* buf, int n) = 0; // the folder shown, or what was searched for
  // A tap on row i: 1 = a folder, now shown; 2 = a track, now open; 0 = neither.
  virtual int trackEnter(int i) = 0;
  // A step back: from the hits to the folder, from a folder to the one above. Returns
  // the row to show there; -1 at the top (nothing to go back to).
  virtual int trackUp() = 0;
  virtual void trackFind(const char* text) = 0;  // the hits are the rows now
  // The list is opened: shows the open track's folder and returns the track's row (with
  // none open: stays where it was, row 0).
  virtual int trackListHome() = 0;
  // The open track and what is done to it.
  virtual void track(TrackView* v) = 0;
  virtual void trackPlay(bool on) = 0;           // play, or pause (it keeps its place)
  virtual void trackStop() = 0;                  // and back to the start
  virtual void trackSeek(float seconds) = 0;
  virtual void trackLoopMark(bool end) = 0;      // the loop's start or end where the track is now
  virtual void trackLoopOff() = 0;
  virtual void trackTempoStep(int percent) = 0;  // slower (< 0) or faster, at the same pitch
  virtual void trackBpmScale(bool twice) = 0;    // the track's own tempo is half / twice what was found
  virtual void trackBarHere() = 0;               // bar 1 starts where the track is now
  virtual void trackVolume(SettingView* v) = 0;  // its level, as a setting
  virtual void trackVolumeStep(int steps) = 0;
};

class Ui {
 public:
  enum Screen { kHome, kPresets, kTuner, kConfirm, kBlockEdit, kFiles, kTrainer, kSaveMenu, kName, kMaster, kDeleteAsk, kChoices,
                kTracks };
  static const int kNameMax = 24;      // a preset name's longest
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
                kListBack, kListOk, kSave, kDiscard, kCancel, kEditBack, kEditOn, kMinus, kPlus, kFlip,
                kEditUp, kEditDown, kFilePrev, kFileNext, kOpenFiles, kFileRow, kFilesBack, kFilesUp, kFilesDown, kOpenTrainer,
                kTrainerOn, kPlay, kOpenSave, kSaveOver, kSaveNew, kKey, kCaps, kSpace, kDelete, kNameOk, kHome_,
                kOpenMaster, kVolDown, kVolUp, kDrag, kDragVol, kRename, kAskDelete,
                kDeleteYes, kTrainerPage, kOpenChoices, kChoiceRow, kChoicesBack, kChoicesUp, kChoicesDown,
                kOpenTracks, kOpenTrackList, kTrackFind, kNameCancel, kTrackPlay, kTrackStop, kTrackA, kTrackB, kTrackLoopOff, kTrackSlower,
                kTrackFaster, kTrackHalf, kTrackDouble, kTrackBar, kTrackVolDown, kTrackVolUp, kDragSeek };
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
  static const int kMaxParts = 96;
  struct Part {
    Rect r;
    uint32_t sig;
  };

  // A part of the screen: true if it must be painted now.
  bool part(Pass& p, Rect r, uint32_t sig);
  void target(Pass& p, Rect r, Action a, int arg = 0);
  bool pressed(Action a, int arg = 0) const { return held_ && inside_ && heldAction_ == a && heldArg_ == arg; }
  // small: the label in the small font (the home screen's bottom line, where five share the width).
  void button(Pass& p, Rect r, const char* label, Action a, int arg = 0, uint16_t colour = 0, bool enabled = true,
              bool small = false);
  void arrowButton(Pass& p, Rect r, int dir, Action a);
  void meter(Pass& p, int x, int y, int labelW, const char* label, float db);

  void layout(Pass& p);
  void layoutHome(Pass& p);
  void layoutHomeDrums(Pass& p);
  void layoutPresets(Pass& p);
  void layoutTuner(Pass& p);
  void layoutConfirm(Pass& p);
  void layoutBlock(Pass& p);
  void layoutFiles(Pass& p);
  void layoutTrackList(Pass& p);
  void layoutTrainer(Pass& p);
  void layoutSaveMenu(Pass& p);
  void layoutName(Pass& p);
  void layoutMaster(Pass& p);
  void layoutDelete(Pass& p);
  void layoutChoices(Pass& p);
  void layoutTracks(Pass& p);
  // A value between minus and plus (the tracks screen's tempo and volume).
  void valueRow(Pass& p, int y, int h, const char* label, const char* text, float fraction, uint16_t colour, Action minus,
                Action plus);
  // One setting with minus and plus in row `row` of the settings area.
  void settingRow(Pass& p, int row, int block, int i, const SettingView& v);
  // The place of a setting in a block by its name in blocks.json; -1 if it has none.
  static int findSetting(int block, const char* ref);
  void fileStep(int dir);
  // The settings of the edited block that are shown now; returns how many.
  int shownSettings(int* idx);

  void fire(Action a, int arg);
  // A finger sliding along a value: sets it from where the slide has got to.
  void dragTo(int x);
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
  // Dragging a value (kDrag, kDragVol): it begins when the finger has moved kDragStart
  // pixels sideways, and the value then follows the finger from where it was, the bar's
  // width being the whole range. Each reading is used one reading late and the last one
  // before the finger lifts is dropped: a resistive panel's point jumps as the pressure goes.
  static const int kDragStart = 10;
  bool dragging_ = false, dragPending_ = false;
  int pressX_ = 0, dragX0_ = 0, dragWidth_ = 1, dragPendingX_ = 0;
  float dragFrom_ = 0.0f;
  // Meters: read 20 times a second, the peak falls slowly.
  uint32_t meterMs_ = 0;
  float inDb_ = -100.0f, outDb_ = -100.0f, limDb_ = 0.0f;
  int cpu_ = 0;
  uint32_t cpuMs_ = 0;
  // Preset list.
  int page_ = 0;
  bool previewing_ = false, editedAtOpen_ = false;
  // A block's settings.
  int editBlock_ = 0, editPage_ = 0, repeats_ = 0, filePage_ = 0;
  // The list of a setting's named choices (the drums' style and song, a type, a mode).
  int choiceSetting_ = 0, choicePage_ = 0;
  Screen choiceBack_ = kBlockEdit;
  int trainerPage_ = 0;  // 0: the trainer's settings, 1: what the drums play
  // The backing track: the file list shows tracks (not a block's files); a finger on the
  // progress bar seeks, at most a few times a second.
  bool trackList_ = false;
  uint32_t nowMs_ = 0, seekMs_ = 0;
  float seekTo_ = -1.0f;  // where the finger has got to and the track has not yet (a part of its length)
  // The new preset's name as it is typed.
  char name_[kNameMax + 1] = "";
  bool upper_ = true, autoUpper_ = true, saveFailed_ = false;
  // What is typed: a new preset's name, the recalled preset's new one, or words to find tracks by.
  enum NamePurpose { kNameSave, kNameRename, kNameFind };
  NamePurpose purpose_ = kNameSave;
  // The question.
  bool askKeep_ = false;
  int askTarget_ = -1;
};

}  // namespace ui_tft
