// Ui.cpp - see Ui.h.
#include <ui/Ui.h>

#include <stdio.h>
#include <string.h>

namespace ui_tft {

namespace {

const uint16_t kBg = rgb(14, 16, 20), kPanel = rgb(30, 34, 42), kPanelHi = rgb(58, 66, 80);
const uint16_t kText = rgb(236, 238, 242), kDim = rgb(138, 146, 158), kOff = rgb(44, 49, 59);
const uint16_t kAmber = rgb(255, 194, 71), kAccent = rgb(91, 140, 255), kDark = rgb(12, 13, 16);

struct Sig {
  uint32_t h = 2166136261u;
  Sig& i(int v) {
    for (int k = 0; k < 4; k++) h = (h ^ ((uint32_t)v >> (8 * k) & 255u)) * 16777619u;
    return *this;
  }
  Sig& s(const char* t) {
    for (; t && *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
    return i(0);
  }
};

const char* const kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// Where each of the meter's twelve segments lights, in dBFS.
const float kSegDb[12] = {-54, -48, -42, -36, -30, -24, -18, -12, -9, -6, -3, -1};

inline Rect R(int x, int y, int w, int h) { return {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h}; }

// A filled triangle pointing left (0), right (1), up (2) or down (3), centred in r.
void triangle(Canvas& c, Rect r, int dir, int size, uint16_t colour) {
  const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  for (int k = 0; k < size; k++) {
    const int half = dir == 0 || dir == 2 ? k : size - 1 - k;  // the row's half width
    if (dir < 2) c.fill(R(cx - size / 2 + k, cy - half, 1, 2 * half + 1), colour);
    else c.fill(R(cx - half, cy - size / 2 + k, 2 * half + 1, 1), colour);
  }
}

// The preset's name without its number ("03 Lead" -> "Lead").
const char* bareName(const char* name, int number) {
  if (number > 0 && name[0] >= '0' && name[0] <= '9') {
    while (*name >= '0' && *name <= '9') name++;
    while (*name == ' ') name++;
  }
  return name;
}

}  // namespace

// ---- Parts ----

bool Ui::part(Pass& p, Rect r, uint32_t sig) {
  if (p.mode == kMeasure) {
    if (p.count < kMaxParts) now_[p.count] = {r, sig};
    p.count++;
    return false;
  }
  return p.mode == kPaint && r.overlaps(p.canvas->window());
}

void Ui::target(Pass& p, Rect r, Action a, int arg) {
  if (p.mode == kHit && r.contains(p.hx, p.hy)) {
    p.action = a;
    p.arg = arg;
    p.rect = r;
  }
}

void Ui::button(Pass& p, Rect r, const char* label, Action a, int arg, uint16_t colour, bool enabled) {
  const bool down = enabled && pressed(a, arg);
  if (part(p, r, Sig().s(label).i(down).i(colour).i(enabled).h)) {
    p.canvas->roundRect(r, 6, down ? kPanelHi : (colour && enabled ? colour : kPanel));
    p.canvas->textIn(r.inset(4), kFontMedium, label, !enabled ? kOff : (colour && !down ? kDark : kText));
  }
  if (enabled) target(p, r, a, arg);
}

void Ui::arrowButton(Pass& p, Rect r, int dir, Action a) {
  const bool down = pressed(a);
  if (part(p, r, Sig().i(dir).i(down).h)) {
    p.canvas->roundRect(r, 6, down ? kPanelHi : kPanel);
    triangle(*p.canvas, r, dir, r.h > 40 ? 18 : 12, kText);
  }
  target(p, r, a);
}

void Ui::meter(Pass& p, int x, int y, int labelW, const char* label, float db) {
  if (part(p, R(x, y, labelW, 20), Sig().s(label).h)) p.canvas->textIn(R(x, y, labelW, 20), kFontSmall, label, kDim, kLeft);
  for (int i = 0; i < 12; i++) {
    const bool lit = db >= kSegDb[i];
    const Rect r = R(x + labelW + i * 7, y + 5, 5, 10);
    if (part(p, r, lit))
      p.canvas->fill(r, !lit ? kOff : (kSegDb[i] >= kMeterHotDb ? kMeterHot : (kSegDb[i] >= kMeterWarnDb ? kMeterWarn : kMeterOk)));
  }
}

// ---- Screens ----

void Ui::layout(Pass& p) {
  // The background is one part that never changes; the others are painted over it.
  if (part(p, R(0, 0, kScreenW, kScreenH), screen_)) p.canvas->fill(p.canvas->window(), kBg);
  switch (screen_) {
    case kHome: layoutHome(p); break;
    case kPresets: layoutPresets(p); break;
    case kTuner: layoutTuner(p); break;
    case kConfirm: layoutConfirm(p); break;
    case kBlockEdit: layoutBlock(p); break;
    case kFiles: layoutFiles(p); break;
  }
}

void Ui::layoutHome(Pass& p) {
  // Top line: levels and CPU.
  meter(p, 6, 0, 20, "IN", inDb_);
  meter(p, 118, 0, 32, "OUT", outDb_);
  char buf[40];
  snprintf(buf, sizeof(buf), "CPU %d%%", cpu_);
  if (part(p, R(240, 0, 76, 20), Sig().s(buf).h)) p.canvas->textIn(R(240, 0, 76, 20), kFontSmall, buf, kDim, kRight);

  // The preset: number, name, previous and next.
  const int cur = m_.presetCurrent();
  const bool edited = cur >= 0 && m_.presetEdited();
  const int number = cur >= 0 ? m_.presetNumber(cur) : 0;
  const char* name = cur >= 0 ? bareName(m_.presetName(cur), number) : "No preset";
  arrowButton(p, R(4, 22, 48, 60), 0, kPrev);
  arrowButton(p, R(268, 22, 48, 60), 1, kNext);
  if (number > 0) snprintf(buf, sizeof(buf), "%02d", number);
  else strcpy(buf, "--");
  const Rect numR = R(56, 22, 208, 60);
  if (part(p, numR, Sig().s(buf).i(edited).i(pressed(kOpenList)).h)) {
    p.canvas->roundRect(numR, 6, pressed(kOpenList) ? kPanelHi : kPanel);
    p.canvas->textIn(numR, kFontHuge, buf, kText);
    if (edited) {
      const Rect pill = R(numR.x + numR.w - 58, numR.y + 5, 54, 16);
      p.canvas->roundRect(pill, 8, kAmber);
      p.canvas->textIn(pill, kFontSmall, "EDITED", kDark);
    }
  }
  const Rect nameR = R(4, 84, 312, 34);
  if (part(p, nameR, Sig().s(name).i(edited).h)) {
    const Font& f = Canvas::textWidth(kFontLarge, name) <= nameR.w ? kFontLarge : kFontMedium;
    p.canvas->textIn(nameR, f, name, edited ? kAmber : kText);
  }
  target(p, R(56, 22, 208, 96), kOpenList);

  // The chain: a tile a block, lit while it is on. The last one (drums and bass) is two wide.
  for (int i = 0; i < kBlockCount; i++) {
    const int col = i % 5, row = i / 5;
    const bool wide = i == kBlockCount - 1 && col < 4;
    const Rect r = R(4 + col * 63, 122 + row * 44, wide ? 60 + 63 * (4 - col) : 60, 40);
    const bool on = m_.blockOn(i), down = pressed(kBlock, i);
    if (part(p, r, Sig().i(i).i(on).i(down).h)) {
      const uint16_t c = kBlocks[i].colour;
      p.canvas->roundRect(r, 6, down ? kPanelHi : (on ? c : kPanel));
      if (!on) p.canvas->roundRect(R(r.x, r.y, 5, r.h), 2, mix(kPanel, c, 3));  // its colour stays as a stripe
      p.canvas->textIn(r.inset(2), kFontSmall, kBlocks[i].label, on && !down ? kDark : kDim);
    }
    target(p, r, kBlock, i);
  }

  button(p, R(4, 210, 101, 30), "TUNER", kOpenTuner);
  button(p, R(109, 210, 102, 30), "SAVE", kSaveNow, 0, kAmber, edited);  // only with something to save
  button(p, R(215, 210, 101, 30), "PRESETS", kOpenList);
}

// The list of models or IRs: a tap loads one at once (the one that plays is lit).
void Ui::layoutFiles(Pass& p) {
  const FileKind kind = kBlocks[editBlock_].file;
  const int count = m_.fileCount(kind), pages = count ? (count + kRowsPerPage - 1) / kRowsPerPage : 1;
  if (filePage_ >= pages) filePage_ = pages - 1;
  char buf[32];
  const char* title = kind == kFileModel ? "AMP MODELS" : "CAB IRS";
  if (part(p, R(8, 0, 150, 24), Sig().s(title).h)) p.canvas->textIn(R(8, 0, 150, 24), kFontSmall, title, kDim, kLeft);
  snprintf(buf, sizeof(buf), "%d / %d", filePage_ + 1, pages);
  if (part(p, R(232, 0, 80, 24), Sig().s(buf).h)) p.canvas->textIn(R(232, 0, 80, 24), kFontSmall, buf, kDim, kRight);
  const int cur = m_.fileCurrent(kind);
  for (int row = 0; row < kRowsPerPage; row++) {
    const int idx = filePage_ * kRowsPerPage + row;
    const Rect r = R(4, 24 + row * 36, 312, 34);
    if (idx >= count) {
      part(p, r, 0);
      continue;
    }
    const char* name = m_.fileName(kind, idx);
    const bool playing = idx == cur, down = pressed(kFileRow, idx);
    if (part(p, r, Sig().i(idx).s(name).i(playing).i(down).h)) {
      p.canvas->roundRect(r, 6, down ? kPanelHi : (playing ? kAccent : kPanel));
      // Long file names get the small font, so more of them shows.
      const Rect in = R(r.x + 10, r.y, r.w - 20, r.h);
      const Font& f = Canvas::textWidth(kFontMedium, name) <= in.w ? kFontMedium : kFontSmall;
      p.canvas->textIn(in, f, name, playing ? kDark : kText, kLeft);
    }
    target(p, r, kFileRow, idx);
  }
  button(p, R(4, 206, 100, 34), "BACK", kFilesBack);
  arrowButton(p, R(108, 206, 102, 34), 2, kFilesUp);
  arrowButton(p, R(214, 206, 102, 34), 3, kFilesDown);
}

void Ui::fileStep(int dir) {
  const FileKind kind = kBlocks[editBlock_].file;
  const int count = m_.fileCount(kind);
  if (kind == kFileNone || !count) return;
  const int cur = m_.fileCurrent(kind);
  m_.fileLoad(kind, cur < 0 ? 0 : (cur + dir + count) % count);
}

int Ui::shownSettings(int* idx) {
  int n = 0;
  SettingView v;
  for (int i = 0; i < kBlocks[editBlock_].settingCount; i++)
    if (m_.setting(editBlock_, i, &v)) idx[n++] = i;
  return n;
}

// A block's settings: four to a page, each with minus and plus (holding repeats, then goes
// faster); a toggle also flips by a tap on its name.
void Ui::layoutBlock(Pass& p) {
  const BlockDef& b = kBlocks[editBlock_];
  const bool on = m_.blockOn(editBlock_);
  button(p, R(4, 2, 64, 28), "BACK", kEditBack);
  if (part(p, R(72, 2, 160, 28), Sig().s(b.name).h)) p.canvas->textIn(R(72, 2, 160, 28), kFontMedium, b.name, b.colour);
  button(p, R(236, 2, 80, 28), on ? "ON" : "OFF", kEditOn, 0, on ? b.colour : 0);

  // The amp and the cab begin with the file they play: previous and next, or a tap for the list.
  const int fileRow = b.file != kFileNone ? 1 : 0;
  int idx[kMaxBlockSettings];
  const int count = shownSettings(idx) + fileRow, pages = count ? (count + kSettingsPerPage - 1) / kSettingsPerPage : 1;
  if (editPage_ >= pages) editPage_ = pages - 1;
  for (int row = 0; row < kSettingsPerPage; row++) {
    const int n = editPage_ * kSettingsPerPage + row - fileRow, y = 34 + row * 43;
    const Rect minus = R(4, y, 48, 40), mid = R(56, y, 208, 40), plus = R(268, y, 48, 40);
    SettingView v;
    if (n < 0) {
      const int cur = m_.fileCurrent(b.file);
      const char* name = cur >= 0 ? m_.fileName(b.file, cur) : "none";
      const bool down = pressed(kOpenFiles);
      arrowButton(p, minus, 0, kFilePrev);
      if (part(p, mid, Sig().s(name).i(b.file).i(down).h)) {
        p.canvas->roundRect(mid, 6, down ? kPanelHi : kPanel);
        p.canvas->textIn(R(mid.x + 8, mid.y + 2, mid.w - 16, 16), kFontSmall, b.file == kFileModel ? "Model" : "IR", kDim, kLeft);
        p.canvas->textIn(R(mid.x + 8, mid.y + 17, mid.w - 16, 20), kFontSmall, name, kText, kLeft);
      }
      target(p, mid, kOpenFiles);
      arrowButton(p, plus, 1, kFileNext);
      continue;
    }
    if (n >= count - fileRow || !m_.setting(editBlock_, idx[n], &v)) {
      part(p, minus, 0);  // the same parts on every page
      part(p, mid, 0);
      part(p, plus, 0);
      continue;
    }
    const int i = idx[n];
    button(p, minus, "-", kMinus, i);
    const bool down = v.toggle && pressed(kFlip, i);
    if (part(p, mid, Sig().i(i).s(v.label).s(v.text).i((int)(v.fraction * 192.0f)).i(v.bar).i(v.toggle && v.on).i(down).h)) {
      p.canvas->roundRect(mid, 6, down ? kPanelHi : kPanel);
      const int textW = Canvas::textWidth(kFontMedium, v.text);
      const Rect line = R(mid.x + 8, mid.y + (v.bar ? 0 : 3), mid.w - 16, 34);
      p.canvas->textIn(R(line.x, line.y, line.w - textW - 8, line.h), kFontSmall, v.label, kDim, kLeft);
      p.canvas->textIn(line, kFontMedium, v.text, v.toggle && !v.on ? kDim : kText, kRight);
      if (v.bar) {
        const float f = v.fraction < 0.0f ? 0.0f : (v.fraction > 1.0f ? 1.0f : v.fraction);
        p.canvas->fill(R(mid.x + 8, mid.y + 32, 192, 3), kOff);
        p.canvas->fill(R(mid.x + 8, mid.y + 32, (int)(f * 192.0f + 0.5f), 3), b.colour);
      }
    }
    if (v.toggle) target(p, mid, kFlip, i);
    button(p, plus, "+", kPlus, i);
  }
  // More than a page: up, where we are, down.
  const bool more = pages > 1;
  char buf[32];
  snprintf(buf, sizeof(buf), "%d / %d", editPage_ + 1, pages);
  if (more) arrowButton(p, R(4, 206, 100, 34), 2, kEditUp);
  else part(p, R(4, 206, 100, 34), 0);
  if (part(p, R(108, 206, 104, 34), more ? Sig().s(buf).h : 0) && more) p.canvas->textIn(R(108, 206, 104, 34), kFontSmall, buf, kDim);
  if (more) arrowButton(p, R(216, 206, 100, 34), 3, kEditDown);
  else part(p, R(216, 206, 100, 34), 0);
}

void Ui::layoutPresets(Pass& p) {
  const int count = m_.presetCount(), pages = count ? (count + kRowsPerPage - 1) / kRowsPerPage : 1;
  if (page_ >= pages) page_ = pages - 1;
  char buf[48];
  if (part(p, R(8, 0, 150, 24), 1)) p.canvas->textIn(R(8, 0, 150, 24), kFontSmall, "PRESETS", kDim, kLeft);
  snprintf(buf, sizeof(buf), "%d / %d", page_ + 1, pages);
  if (part(p, R(232, 0, 80, 24), Sig().s(buf).h)) p.canvas->textIn(R(232, 0, 80, 24), kFontSmall, buf, kDim, kRight);

  const int cur = m_.presetCurrent(), saved = m_.presetSaved();
  for (int row = 0; row < kRowsPerPage; row++) {
    const int idx = page_ * kRowsPerPage + row;
    const Rect r = R(4, 24 + row * 36, 312, 34);
    if (idx >= count) {
      part(p, r, 0);  // keeps the parts' order the same on every page
      continue;
    }
    const int number = m_.presetNumber(idx);
    const char* name = bareName(m_.presetName(idx), number);
    const bool playing = idx == cur, down = pressed(kRow, idx);
    const bool mark = idx == saved && previewing_ && idx != cur;  // where Back returns to
    if (part(p, r, Sig().i(idx).s(name).i(number).i(playing).i(down).i(mark).h)) {
      p.canvas->roundRect(r, 6, down ? kPanelHi : (playing ? kAccent : kPanel));
      if (number > 0) snprintf(buf, sizeof(buf), "%02d", number);
      else strcpy(buf, "--");
      p.canvas->textIn(R(r.x + 10, r.y, 30, r.h), kFontMedium, buf, playing ? kDark : kDim, kLeft);
      p.canvas->textIn(R(r.x + 46, r.y, r.w - 56 - (mark ? 60 : 0), r.h), kFontMedium, name, playing ? kDark : kText, kLeft);
      if (mark) p.canvas->textIn(R(r.x + r.w - 66, r.y, 58, r.h), kFontSmall, "BEFORE", kDim, kRight);
    }
    target(p, r, kRow, idx);
  }
  button(p, R(4, 206, 75, 34), "BACK", kListBack);
  arrowButton(p, R(83, 206, 75, 34), 2, kPageUp);
  arrowButton(p, R(162, 206, 75, 34), 3, kPageDown);
  button(p, R(241, 206, 75, 34), "OK", kListOk, 0, kMeterOk);
}

void Ui::layoutTuner(Pass& p) {
  const TunerView t = m_.tuner();
  if (part(p, R(8, 0, 150, 24), 1)) p.canvas->textIn(R(8, 0, 150, 24), kFontSmall, "TUNER", kDim, kLeft);
  char buf[32];
  // The reading is rounded to whole cents so the screen is not repainted for noise.
  const int cents = t.valid ? (int)(t.cents + (t.cents < 0 ? -0.5f : 0.5f)) : 0;
  const bool inTune = t.valid && cents >= -3 && cents <= 3;
  const uint16_t colour = !t.valid ? kDim : (inTune ? kMeterOk : kText);

  const Rect noteR = R(60, 26, 200, 96);
  snprintf(buf, sizeof(buf), "%s", t.valid ? kNotes[((t.note % 12) + 12) % 12] : "-");
  if (part(p, noteR, Sig().s(buf).i(colour).h)) p.canvas->textIn(noteR, kFontHuge, buf, colour);

  // The needle: 2 pixels a cent, a mark in the middle.
  const Rect track = R(20, 132, 280, 26);
  const int pos = t.valid ? (cents < -50 ? -50 : (cents > 50 ? 50 : cents)) : 0;
  if (part(p, track, Sig().i(pos).i(t.valid).i(inTune).h)) {
    p.canvas->roundRect(track, 6, kPanel);
    p.canvas->fill(R(track.x + track.w / 2 - 1, track.y, 2, track.h), kDim);
    if (t.valid) p.canvas->roundRect(R(track.x + track.w / 2 + pos * 2.6f - 6, track.y + 3, 12, track.h - 6), 4, inTune ? kMeterOk : kAmber);
  }
  if (t.valid) snprintf(buf, sizeof(buf), "%+d cents", cents);
  else strcpy(buf, "Play a string");
  if (part(p, R(20, 164, 280, 22), Sig().s(buf).h)) p.canvas->textIn(R(20, 164, 280, 22), kFontMedium, buf, kText);
  if (t.valid) snprintf(buf, sizeof(buf), "%.1f Hz", (double)t.hz);
  else buf[0] = 0;
  if (part(p, R(20, 186, 280, 18), Sig().s(buf).h)) p.canvas->textIn(R(20, 186, 280, 18), kFontSmall, buf, kDim);
  button(p, R(4, 206, 312, 34), "BACK", kCloseTuner);
}

void Ui::layoutConfirm(Pass& p) {
  const int saved = m_.presetSaved();
  if (part(p, R(10, 40, 300, 30), 1)) p.canvas->textIn(R(10, 40, 300, 30), kFontLarge, "Unsaved changes", kText);
  char buf[64];
  if (saved >= 0) snprintf(buf, sizeof(buf), "to %s", m_.presetName(saved));
  else buf[0] = 0;
  if (part(p, R(10, 82, 300, 26), Sig().s(buf).h)) p.canvas->textIn(R(10, 82, 300, 26), kFontMedium, buf, kAmber);
  button(p, R(10, 140, 96, 52), "SAVE", kSave, 0, kMeterOk);
  button(p, R(112, 140, 96, 52), "DISCARD", kDiscard);
  button(p, R(214, 140, 96, 52), "CANCEL", kCancel);
}

// ---- Actions ----

void Ui::go(Screen s) {
  if (screen_ == kTuner && s != kTuner) m_.tunerOpen(false);
  screen_ = s;
}

void Ui::ask(bool keepPreview, int target) {
  askKeep_ = keepPreview;
  askTarget_ = target;
  back_ = screen_;
  go(kConfirm);
}

void Ui::step(int dir) {
  const int count = m_.presetCount();
  if (!count) return;
  const int cur = m_.presetCurrent();
  const int to = cur < 0 ? 0 : (cur + dir + count) % count;
  if (m_.presetEdited()) ask(false, to);
  else m_.recall(to);
}

void Ui::fire(Action a, int arg) {
  switch (a) {
    case kPrev: step(-1); break;
    case kNext: step(1); break;
    case kBlock: m_.toggleBlock(arg); break;
    case kOpenTuner:
      m_.tunerOpen(true);
      go(kTuner);
      break;
    case kCloseTuner: go(kHome); break;
    case kOpenList: {
      const int cur = m_.presetCurrent();
      page_ = cur > 0 ? cur / kRowsPerPage : 0;
      previewing_ = false;
      editedAtOpen_ = m_.presetEdited();
      go(kPresets);
      break;
    }
    case kRow:
      if (arg != m_.presetCurrent() || !previewing_) previewing_ = m_.preview(arg) || previewing_;
      break;
    case kPageUp:
      if (page_ > 0) page_--;
      break;
    case kPageDown:
      if ((page_ + 1) * kRowsPerPage < m_.presetCount()) page_++;
      break;
    case kListBack:
      if (previewing_) m_.cancelPreview();
      previewing_ = false;
      go(kHome);
      break;
    case kListOk:
      // Keeping another preset drops the edits made before the list was opened: ask first.
      if (previewing_ && editedAtOpen_ && m_.presetCurrent() != m_.presetSaved()) {
        ask(true, m_.presetCurrent());
        break;
      }
      if (previewing_) m_.keepPreview();
      previewing_ = false;
      go(kHome);
      break;
    case kSave:
    case kDiscard:
      if (askKeep_) {
        if (a == kSave) {  // back to the edited sound, save it, then the chosen preset
          m_.cancelPreview();
          m_.save();
          m_.recall(askTarget_);
        } else {
          m_.keepPreview();
        }
        previewing_ = false;
      } else {
        if (a == kSave) m_.save();
        m_.recall(askTarget_);
      }
      go(kHome);
      break;
    case kCancel: go(back_); break;
    case kSaveNow:
      if (m_.presetEdited()) m_.save();
      break;
    case kEditBack: go(kHome); break;
    case kEditOn: m_.toggleBlock(editBlock_); break;
    case kMinus: m_.settingStep(editBlock_, arg, repeats_ > 10 ? -5 : -1); break;
    case kPlus: m_.settingStep(editBlock_, arg, repeats_ > 10 ? 5 : 1); break;
    case kFlip: {
      SettingView v;
      if (m_.setting(editBlock_, arg, &v)) m_.settingStep(editBlock_, arg, v.on ? -1 : 1);
      break;
    }
    case kEditUp:
      if (editPage_ > 0) editPage_--;
      break;
    case kEditDown: {
      int idx[kMaxBlockSettings];
      if ((editPage_ + 1) * kSettingsPerPage < shownSettings(idx) + (kBlocks[editBlock_].file != kFileNone)) editPage_++;
      break;
    }
    case kFilePrev: fileStep(-1); break;
    case kFileNext: fileStep(1); break;
    case kOpenFiles: {
      const int cur = m_.fileCurrent(kBlocks[editBlock_].file);
      filePage_ = cur > 0 ? cur / kRowsPerPage : 0;
      go(kFiles);
      break;
    }
    case kFileRow:
      if (arg != m_.fileCurrent(kBlocks[editBlock_].file)) m_.fileLoad(kBlocks[editBlock_].file, arg);
      break;
    case kFilesBack: go(kBlockEdit); break;
    case kFilesUp:
      if (filePage_ > 0) filePage_--;
      break;
    case kFilesDown:
      if ((filePage_ + 1) * kRowsPerPage < m_.fileCount(kBlocks[editBlock_].file)) filePage_++;
      break;
    case kNone: break;
  }
}

// ---- Touch ----

void Ui::touch(bool down, int x, int y, uint32_t ms) {
  if (down) {
    lastDownMs_ = ms;
    if (!held_) {
      Pass p;
      p.mode = kHit;
      p.hx = x;
      p.hy = y;
      layout(p);
      held_ = true;
      inside_ = p.action != kNone;
      repeated_ = false;
      repeats_ = 0;
      heldAction_ = p.action;
      heldArg_ = p.arg;
      heldRect_ = p.rect;
      pressMs_ = repeatMs_ = ms;
    } else if (heldAction_ != kNone) {
      // Sliding off the button lets it go; a resistive panel wanders a little, so the
      // button counts as 8 pixels larger while it is held.
      const Rect grown = {(int16_t)(heldRect_.x - 8), (int16_t)(heldRect_.y - 8), (int16_t)(heldRect_.w + 16), (int16_t)(heldRect_.h + 16)};
      inside_ = grown.contains(x, y);
    }
    return;
  }
  // Up: only after 40 ms without contact (the panel bounces).
  if (!held_ || ms - lastDownMs_ < 40) return;
  held_ = false;
  if (inside_ && !repeated_) fire(heldAction_, heldArg_);
  heldAction_ = kNone;
}

void Ui::tick(uint32_t ms) {
  // Holding previous / next or a page button repeats: after 0.6 s, then every 0.3 s.
  if (held_ && inside_ && (heldAction_ == kPrev || heldAction_ == kNext || heldAction_ == kPageUp || heldAction_ == kPageDown) &&
      screen_ != kConfirm && ms - pressMs_ >= 600 && ms - repeatMs_ >= 300) {
    repeatMs_ = ms;
    repeated_ = true;
    fire(heldAction_, heldArg_);
  }
  // Holding minus or plus: after 0.4 s ten single steps a second, after the tenth five at a time.
  if (held_ && inside_ && (heldAction_ == kMinus || heldAction_ == kPlus) && screen_ == kBlockEdit &&
      ms - pressMs_ >= 400 && ms - repeatMs_ >= 100) {
    repeatMs_ = ms;
    repeated_ = true;
    repeats_++;
    fire(heldAction_, heldArg_);
  }
  // Holding a block's tile for half a second opens its settings (a tap switches it).
  if (held_ && inside_ && heldAction_ == kBlock && screen_ == kHome && !repeated_ && ms - pressMs_ >= 500) {
    repeated_ = true;
    editBlock_ = heldArg_;
    editPage_ = 0;
    go(kBlockEdit);
  }
  if (ms - meterMs_ >= 50) {
    // The meters: up at once, down 1.5 dB a reading (30 dB a second).
    meterMs_ = ms;
    const float in = m_.inputDb(), out = m_.outputDb();
    inDb_ = in > inDb_ - 1.5f ? in : inDb_ - 1.5f;
    outDb_ = out > outDb_ - 1.5f ? out : outDb_ - 1.5f;
    if (inDb_ < -100.0f) inDb_ = -100.0f;
    if (outDb_ < -100.0f) outDb_ = -100.0f;
  }
  if (ms - cpuMs_ >= 500) {
    cpuMs_ = ms;
    cpu_ = m_.cpuPercent();
  }
}

// ---- Painting ----

bool Ui::render(uint16_t* buf, int bufPixels, Rect* out) {
  if (!painting_) {
    Pass p;
    p.mode = kMeasure;
    layout(p);
    const int count = p.count < kMaxParts ? p.count : kMaxParts;
    if (drawnCount_ != count || drawnScreen_ != screen_) {
      // Another screen: everything.
      memcpy(drawn_, now_, sizeof(Part) * count);
      drawnCount_ = count;
      drawnScreen_ = screen_;
      dirty_ = {0, 0, kScreenW, kScreenH};
    } else {
      int i = 1;  // part 0 is the background
      while (i < count && now_[i].sig == drawn_[i].sig) i++;
      if (i == count) return false;
      drawn_[i] = now_[i];
      dirty_ = now_[i].r;
    }
    painting_ = true;
    dirtyY_ = dirty_.y;
  }
  int rows = bufPixels / dirty_.w;
  const int left = dirty_.y + dirty_.h - dirtyY_;
  if (rows > left) rows = left;
  *out = {dirty_.x, (int16_t)dirtyY_, dirty_.w, (int16_t)rows};
  Canvas c(buf, *out);
  Pass p;
  p.mode = kPaint;
  p.canvas = &c;
  layout(p);
  dirtyY_ += rows;
  if (dirtyY_ >= dirty_.y + dirty_.h) painting_ = false;
  return true;
}

}  // namespace ui_tft
