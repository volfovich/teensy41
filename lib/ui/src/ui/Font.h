// Font.h - the touch screen's fonts: glyphs with 2-bit coverage, made by tools/make_font.py.
#pragma once

#include <stdint.h>

namespace ui_tft {

struct Glyph {
  uint32_t offset;  // into the font's bits
  uint8_t w, h;     // the glyph's rectangle
  int8_t dx, dy;    // its place from the pen position and the line's top
  uint8_t advance;
};

struct Font {
  const uint8_t* bits;
  const Glyph* glyphs;
  const char* chars;  // the characters it has, in order; nullptr = ASCII 32..126
  uint8_t count, height, ascent;

  // The glyph for c, or nullptr if the font has none.
  const Glyph* glyph(char c) const {
    if (!chars) return c >= 32 && c < 32 + count ? &glyphs[c - 32] : nullptr;
    for (int i = 0; i < count; i++)
      if (chars[i] == c) return &glyphs[i];
    return nullptr;
  }
};

extern const Font kFontSmall;   // 13 px: labels, meters
extern const Font kFontMedium;  // 19 px: buttons, list rows
extern const Font kFontLarge;   // 30 px: the preset's name
extern const Font kFontHuge;    // 76 px: the preset's number, the tuner's note (0-9 A-G # -)

}  // namespace ui_tft
