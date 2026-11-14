// BlocksGen.h - made by tools/make_blocks_header.py from protocol/blocks.json; do not edit.
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
  const char* label;   // the block's short name
  const char* tile;    // a shorter one where it is needed: the home screen's narrow tiles
  const char* name;    // the editor's title
  uint16_t colour;     // RGB565
  const char* onKey;   // the serial key of the block's switch
  bool onInverted;     // the switch is a bypass
  const BlockSetting* settings;
  int settingCount;
  FileKind file;
};

static const BlockSetting kAmpSettings[] = {
    {"amp in", nullptr, nullptr},
    {"amp bass", nullptr, nullptr},
    {"amp mid", nullptr, nullptr},
    {"amp treble", nullptr, nullptr},
    {"amp presence", nullptr, nullptr},
    {"amp out", nullptr, nullptr},
    {"amp cal", nullptr, nullptr},
};

static const BlockSetting kGateSettings[] = {
    {"gate type", nullptr, nullptr},
    {"gate thr", nullptr, nullptr},
    {"gate rel", "gate type", "|hard|"},
};

static const BlockSetting kCabSettings[] = {
    {"g", nullptr, nullptr},
    {"pvol", nullptr, nullptr},
};

static const BlockSetting kModSettings[] = {
    {"mod type", nullptr, nullptr},
    {"mod rate", nullptr, nullptr},
    {"mod depth", "mod type", "|chorus|flanger|ce2|"},
    {"mod fb", "mod type", "|chorus|flanger|"},
    {"mod mix", "mod type", "|chorus|flanger|"},
    {"mod width", "mod type", "|chorus|flanger|"},
};

static const BlockSetting kDelaySettings[] = {
    {"dtype", nullptr, nullptr},
    {"dmix", nullptr, nullptr},
    {"dtime", nullptr, nullptr},
    {"dfb", nullptr, nullptr},
    {"dtone", nullptr, nullptr},
    {"dmod", nullptr, nullptr},
    {"dduck", "dtype", "|digital|"},
    {"ddrive", "dtype", "|digital|tape|"},
    {"dera", "dtype", "|digital|"},
};

static const BlockSetting kReverbSettings[] = {
    {"rmode", nullptr, nullptr},
    {"mix", nullptr, nullptr},
    {"size", nullptr, nullptr},
    {"tone", nullptr, nullptr},
    {"rpre", "rmode", "|hall|gated|plate|"},
    {"rmod", "rmode", "|hall|plate|"},
    {"rera", "rmode", "|hall|gated|"},
    {"rgate", "rmode", "|gated|"},
};

static const BlockSetting kLimiterSettings[] = {
    {"lim ceil", nullptr, nullptr},
    {"lim rel", nullptr, nullptr},
    {"lim look", nullptr, nullptr},
};

static const BlockSetting kDrumsSettings[] = {
    {"bpm", nullptr, nullptr},
    {"drum style", nullptr, nullptr},
    {"drum kit", nullptr, nullptr},
    {"drum song", nullptr, nullptr},
    {"drum vol", nullptr, nullptr},
    {"drum comp", nullptr, nullptr},
    {"drum crash", nullptr, nullptr},
    {"drum count", nullptr, nullptr},
    {"@Loop from", nullptr, nullptr},
    {"@Loop to", nullptr, nullptr},
    {"drum train", nullptr, nullptr},
    {"train step", nullptr, nullptr},
    {"train every", nullptr, nullptr},
    {"train target", nullptr, nullptr},
    {"bass", nullptr, nullptr},
    {"bass vol", nullptr, nullptr},
    {"bass key", nullptr, nullptr},
    {"bass tone", nullptr, nullptr},
};

static const BlockDef kBlocks[] = {
    {"amp", "AMP", "AMP", "Amp", 0xFC47, "amp", false, kAmpSettings, 7, kFileModel},
    {"gate", "GATE", "GATE", "Noise gate", 0x8EAA, "gate", false, kGateSettings, 3, kFileNone},
    {"cab", "CAB", "CAB", "Cab", 0xD54B, "b", true, kCabSettings, 2, kFileIr},
    {"mod", "MOD", "MOD", "Mod", 0x3EB4, "mod", false, kModSettings, 6, kFileNone},
    {"delay", "DELAY", "DLY", "Delay", 0x5C7F, "dl", false, kDelaySettings, 9, kFileNone},
    {"reverb", "REVERB", "REV", "Reverb", 0xB3FF, "rv", false, kReverbSettings, 8, kFileNone},
    {"limiter", "LIMIT", "LIMIT", "Limiter", 0xE68D, "lim", false, kLimiterSettings, 3, kFileNone},
    {"drums", "DRUMS + BASS", "DRUMS", "Drums and bass", 0xFAEF, "drum", false, kDrumsSettings, 18, kFileNone},
};
static const int kBlockCount = 8;
static const int kMaxBlockSettings = 18;

static const float kMeterFloorDb = -60.0f, kMeterWarnDb = -6.0f, kMeterHotDb = -1.0f;
static const uint16_t kMeterOk = 0x3EF0, kMeterWarn = 0xFE08, kMeterHot = 0xFA69;

}  // namespace ui_tft
