#!/usr/bin/env python3
"""make_font.py - turns a TTF into the touch screen's glyph tables (lib/ui/src/ui/Fonts.cpp).

Each glyph is a rectangle of 2-bit coverage values (0 = background .. 3 = ink), four to a
byte, rows packed without padding. The screen's Canvas blends them over what is behind.

The tables are checked in; run this only to change a font or a size. Needs Pillow:
    python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install pillow
    /tmp/fontenv/bin/python tools/make_font.py <folder with Inter-Medium.ttf, Inter-SemiBold.ttf, Inter-Bold.ttf>
The fonts are Inter 4.1 (https://github.com/rsms/inter, SIL Open Font License 1.1), from
the release's extras/ttf folder.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ASCII = "".join(chr(c) for c in range(32, 127))
# name, file, pixel size, characters
FONTS = [
    ("kFontSmall", "Inter-Medium.ttf", 13, ASCII),
    ("kFontMedium", "Inter-SemiBold.ttf", 19, ASCII),
    ("kFontLarge", "Inter-Bold.ttf", 30, ASCII),
    ("kFontHuge", "Inter-Bold.ttf", 76, "0123456789ABCDEFG#-"),
]


def build(name, path, size, chars):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    bits, glyphs = bytearray(), []
    for ch in chars:
        x0, y0, x1, y1 = font.getbbox(ch)
        w, h = max(x1 - x0, 0), max(y1 - y0, 0)
        adv = int(round(font.getlength(ch)))
        offset = len(bits)
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255)
            px = [(v + 42) // 85 for v in im.tobytes()]
            for i in range(0, len(px), 4):
                q = px[i:i + 4] + [0] * (4 - len(px[i:i + 4]))
                bits.append(q[0] | q[1] << 2 | q[2] << 4 | q[3] << 6)
        else:
            w = h = 0
        glyphs.append((offset, w, h, x0, y0, adv))
    out = ["static const uint8_t %sBits[] = {" % name]
    for i in range(0, len(bits), 24):
        out.append("  " + ",".join(str(b) for b in bits[i:i + 24]) + ",")
    out.append("};")
    out.append("static const Glyph %sGlyphs[] = {" % name)
    for ch, g in zip(chars, glyphs):
        out.append("  {%d, %d, %d, %d, %d, %d},  // %s" % (g + (repr(ch),)))
    out.append("};")
    listed = "nullptr" if chars == ASCII else '"%s"' % chars
    out.append("const Font %s = {%sBits, %sGlyphs, %s, %d, %d, %d};  // %s %d px, %d bytes"
               % (name, name, name, listed, len(chars), ascent + descent, ascent, os.path.basename(path), size, len(bits)))
    return "\n".join(out), len(bits) + 8 * len(glyphs)


def main():
    folder = sys.argv[1]
    parts, total = [], 0
    for name, file, size, chars in FONTS:
        text, n = build(name, os.path.join(folder, file), size, chars)
        parts.append(text)
        total += n
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "../lib/ui/src/ui/Fonts.cpp"), "w") as f:
        f.write("// Fonts.cpp - made by tools/make_font.py; do not edit. Glyphs of Inter 4.1\n"
                "// (https://github.com/rsms/inter), Copyright 2016 The Inter Project Authors, used under the\n"
                "// SIL Open Font License 1.1. %d bytes in all.\n"
                "#include <ui/Font.h>\n\nnamespace ui_tft {\n\n" % total)
        f.write("\n\n".join(parts))
        f.write("\n\n}  // namespace ui_tft\n")
    print("Fonts.cpp: %d bytes of tables" % total)


if __name__ == "__main__":
    main()
