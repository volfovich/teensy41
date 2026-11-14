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
    rows, tables, most = [], [], 0
    for b in d["blocks"] + [d["lane"]]:
        only = b.get("only", {})
        entries = []
        for section in b["sections"]:
            for ref in section["settings"]:
                if ref in only:
                    (other, choices), = only[ref].items()
                    entries.append('    {"%s", "%s", "|%s|"},' % (ref, other, "|".join(choices)))
                else:
                    entries.append('    {"%s", nullptr, nullptr},' % ref)
        most = max(most, len(entries))
        name = "k%sSettings" % b["id"].capitalize()
        tables.append("static const BlockSetting %s[] = {\n%s\n};" % (name, "\n".join(entries)))
        rows.append('    {"%s", "%s", "%s", 0x%04X, "%s", %s, %s, %d, %s},' % (
            b["id"], b["short"], b["name"], rgb565(b["colour"]), b["on"], "true" if b.get("onInverted") else "false",
            name, len(entries), {"model": "kFileModel", "ir": "kFileIr"}.get(b.get("file"), "kFileNone")))
    m = d["meters"]
    return """// BlocksGen.h - made by tools/make_blocks_header.py from protocol/blocks.json; do not edit.
#pragma once

#include <stdint.h>

namespace ui_tft {

// A setting of a block: its serial key, or "@Name" for one without a key. With onlyRef the
// setting is shown only while that other setting has one of onlyChoices ("|a|b|").
struct BlockSetting {
  const char* ref;
  const char* onlyRef;
  const char* onlyChoices;
};

// The file a block loads and shows, if any.
enum FileKind { kFileModel, kFileIr, kFileNone };

struct BlockDef {
  const char* id;
  const char* label;   // the tile's text
  const char* name;    // the editor's title
  uint16_t colour;     // RGB565
  const char* onKey;   // the serial key of the block's switch
  bool onInverted;     // the switch is a bypass
  const BlockSetting* settings;
  int settingCount;
  FileKind file;
};

%s

static const BlockDef kBlocks[] = {
%s
};
static const int kBlockCount = %d;
static const int kMaxBlockSettings = %d;

static const float kMeterFloorDb = %sf, kMeterWarnDb = %sf, kMeterHotDb = %sf;
static const uint16_t kMeterOk = 0x%04X, kMeterWarn = 0x%04X, kMeterHot = 0x%04X;

}  // namespace ui_tft
""" % ("\n\n".join(tables), "\n".join(rows), len(rows), most, float(m["floorDb"]), float(m["warnDb"]), float(m["hotDb"]),
       rgb565(m["ok"]), rgb565(m["warn"]), rgb565(m["hot"]))


if __name__ == "__main__":
    text = make()
    if "--check" in sys.argv:
        ok = os.path.exists(OUT) and open(OUT).read() == text
        print("BlocksGen.h is %s" % ("current" if ok else "OUT OF DATE: run tools/make_blocks_header.py"))
        sys.exit(0 if ok else 1)
    open(OUT, "w").write(text)
    print("wrote", os.path.normpath(OUT))
