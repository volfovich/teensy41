// Canvas.h - drawing for the touch screen. A Canvas is a window of RGB565 pixels somewhere
// on the 320 x 240 screen (a strip a few rows high); everything is drawn in screen
// coordinates and clipped to the window, so a screen can be painted strip by strip without
// a full frame buffer. Portable: the firmware sends the strip to the display, the host
// tests collect strips into a picture.
#pragma once

#include <stdint.h>

#include <ui/Font.h>

namespace ui_tft {

static const int kScreenW = 320, kScreenH = 240;

struct Rect {
  int16_t x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  bool overlaps(const Rect& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
  Rect inset(int d) const { return {(int16_t)(x + d), (int16_t)(y + d), (int16_t)(w - 2 * d), (int16_t)(h - 2 * d)}; }
};

constexpr uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

// a quarters of fg over bg (0 = bg, 4 = fg).
inline uint16_t mix(uint16_t bg, uint16_t fg, int a) {
  const int r = ((bg >> 11) * (4 - a) + (fg >> 11) * a) >> 2;
  const int g = (((bg >> 5) & 63) * (4 - a) + ((fg >> 5) & 63) * a) >> 2;
  const int b = ((bg & 31) * (4 - a) + (fg & 31) * a) >> 2;
  return (uint16_t)(r << 11 | g << 5 | b);
}

enum Align { kLeft, kCentre, kRight };

class Canvas {
 public:
  Canvas(uint16_t* pixels, Rect window) : px_(pixels), win_(window) {}
  const Rect& window() const { return win_; }

  void fill(Rect r, uint16_t colour);
  // A filled rectangle with rounded, smoothed corners.
  void roundRect(Rect r, int radius, uint16_t colour);
  // Text with its line's top at y; returns the x after the last glyph. Characters the font
  // lacks are skipped.
  int text(const Font& f, int x, int y, const char* s, uint16_t colour);
  // One line of text placed in r (centred vertically), shortened with ".." if it is too wide.
  void textIn(Rect r, const Font& f, const char* s, uint16_t colour, Align align = kCentre);

  static int textWidth(const Font& f, const char* s);
  // Copies s into out, shortened with ".." so that it is at most maxW wide. True if shortened.
  static bool fitText(const Font& f, const char* s, int maxW, char* out, int n);

 private:
  inline void blend(int x, int y, uint16_t colour, int quarters) {
    if (x < win_.x || x >= win_.x + win_.w || y < win_.y || y >= win_.y + win_.h) return;
    uint16_t& p = px_[(y - win_.y) * win_.w + (x - win_.x)];
    p = quarters >= 4 ? colour : mix(p, colour, quarters);
  }
  uint16_t* px_;
  Rect win_;
};

}  // namespace ui_tft
