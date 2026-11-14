#!/usr/bin/env python3
"""ui_render.py - the touch screen's screens as PNG files, drawn by the same code the unit
runs (lib/ui), with a made-up unit behind it:
    pio test -e native -f test_ui && python3 tools/ui_render.py [folder] [scale]
The test writes raw pixels to .pio/ui/; this turns them into <folder>/*.png (default
.pio/ui, scale 2). No libraries needed."""
import glob
import os
import struct
import sys
import zlib

W, H = 320, 240


def png(path, rgb, w, h):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "../.pio/ui")
    out = sys.argv[1] if len(sys.argv) > 1 else src
    scale = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    os.makedirs(out, exist_ok=True)
    files = sorted(glob.glob(os.path.join(src, "*.rgb565")))
    if not files:
        sys.exit("No pictures in .pio/ui: run  pio test -e native -f test_ui  first.")
    for path in files:
        px = struct.unpack("<%dH" % (W * H), open(path, "rb").read())
        rows = []
        for y in range(H):
            row = bytearray()
            for x in range(W):
                v = px[y * W + x]
                r, g, b = v >> 11, (v >> 5) & 63, v & 31
                row += bytes(((r << 3 | r >> 2), (g << 2 | g >> 4), (b << 3 | b >> 2))) * scale
            rows += [bytes(row)] * scale
        name = os.path.join(out, os.path.basename(path)[:-7] + ".png")
        png(name, b"".join(rows), W * scale, H * scale)
        print(os.path.normpath(name))


if __name__ == "__main__":
    main()
