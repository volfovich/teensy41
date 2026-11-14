// BlocksGen.h - made by tools/make_blocks_header.py from protocol/blocks.json; do not edit.
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
    {"amp", "AMP", 0xFC47, "amp", false},
    {"gate", "GATE", 0x8EAA, "gate", false},
    {"cab", "CAB", 0xD54B, "b", true},
    {"doubler", "DBL", 0x3E5B, "dbl", false},
    {"mod", "MOD", 0x3EB4, "mod", false},
    {"delay", "DELAY", 0x5C7F, "dl", false},
    {"reverb", "REVERB", 0xB3FF, "rv", false},
    {"limiter", "LIMIT", 0xE68D, "lim", false},
    {"drums", "DRUMS + BASS", 0xFAEF, "drum", false},
};
static const int kBlockCount = 9;

static const float kMeterFloorDb = -60.0f, kMeterWarnDb = -6.0f, kMeterHotDb = -1.0f;
static const uint16_t kMeterOk = 0x3EF0, kMeterWarn = 0xFE08, kMeterHot = 0xFA69;

}  // namespace ui_tft
