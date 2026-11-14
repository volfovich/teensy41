#!/usr/bin/env python3
"""make_blocks_header.py - writes lib/ui/src/ui/BlocksGen.h from protocol/blocks.json, so the
touch screen shows the chain's blocks with the names, colours and switches the app uses.
Run after changing blocks.json; test/test_ui checks that the header matches the file.
With --check it only compares and exits 1 if the header is out of date."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "../protocol/blocks.json")
OUT = os.path.join(HERE, "../lib/ui/src/ui/BlocksGen.h")


def rgb565(colour):
    r, g, b = int(colour[1:3], 16), int(colour[3:5], 16), int(colour[5:7], 16)
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def make():
    d = json.load(open(SRC))
    rows = []
    for b in d["blocks"] + [d["lane"]]:
        rows.append('    {"%s", "%s", 0x%04X, "%s", %s},' % (
            b["id"], b["short"], rgb565(b["colour"]), b["on"], "true" if b.get("onInverted") else "false"))
    m = d["meters"]
    return """// BlocksGen.h - made by tools/make_blocks_header.py from protocol/blocks.json; do not edit.
#pragma once

#include <stdint.h>

namespace ui_tft {

struct BlockDef {
  const char* id;
  const char* label;   // the tile's text
  uint16_t colour;     // RGB565
  const char* onKey;   // the serial key of the block's switch
  bool onInverted;     // the switch is a bypass
};

static const BlockDef kBlocks[] = {
%s
};
static const int kBlockCount = %d;

static const float kMeterFloorDb = %sf, kMeterWarnDb = %sf, kMeterHotDb = %sf;
static const uint16_t kMeterOk = 0x%04X, kMeterWarn = 0x%04X, kMeterHot = 0x%04X;

}  // namespace ui_tft
""" % ("\n".join(rows), len(rows), float(m["floorDb"]), float(m["warnDb"]), float(m["hotDb"]),
       rgb565(m["ok"]), rgb565(m["warn"]), rgb565(m["hot"]))


if __name__ == "__main__":
    text = make()
    if "--check" in sys.argv:
        ok = os.path.exists(OUT) and open(OUT).read() == text
        print("BlocksGen.h is %s" % ("current" if ok else "OUT OF DATE: run tools/make_blocks_header.py"))
        sys.exit(0 if ok else 1)
    open(OUT, "w").write(text)
    print("wrote", os.path.normpath(OUT))
