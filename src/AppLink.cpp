// AppLink.cpp - see AppLink.h. Milestones A1 to A5 of docs/design/APP_PLAN.md: the session, the settings table,
// the state, setting a value, change events, the meter frames, presets, the model and IR
// lists, sending files to the SD card, (A5) the tuner and the drums' names, and reading
// files and folders back for backups. 16 Nov 2026: the backing track (its lists, opening,
// commands and state) and renaming and removing below /TRACKS.
#include "AppLink.h"

#include <Arduino.h>
#include <SD.h>
#include <applink/Frame.h>
#include <applink/Messages.h>
#include <applink/Wire.h>

#include "AmpLibrary.h"
#include "AudioChain.h"
#include "Meters.h"
#include "DrumSongs.h"
#include "IRLibrary.h"
#include "Params.h"
#include "PresetLibrary.h"
#include "TrackLibrary.h"
#include "Tuner.h"
#include <dsp/drums/DrumKit.h>
#include <dsp/drums/DrumMachine.h>
#include <dsp/player/TrackIndex.h>
#include "config.h"

using applink::Err;
using applink::Msg;
using applink::Reader;
using applink::Writer;
using namespace ui;

// Buffers in the PSRAM: only loop() touches them, and RAM1 and RAM2 are tight.
static constexpr uint32_t kRxPayload = 16384;
static constexpr uint32_t kTxPayload = 16384;
EXTMEM static uint8_t g_rx[kRxPayload];
EXTMEM static uint8_t g_txPayload[kTxPayload];
EXTMEM static uint8_t g_txFrame[kTxPayload + applink::kOverhead];

static constexpr uint32_t kLineIdleMs = 100;       // PROTOCOL.md 2.3
static constexpr uint32_t kSessionTimeoutMs = 5000;
static constexpr uint32_t kChangePollMs = 30;

static applink::FrameParser g_parser(g_rx, kRxPayload);
static uint32_t g_lastByteMs = 0;
static bool g_session = false;
static uint32_t g_lastFrameMs = 0;

// What the app was last told, to find changes.
static int16_t g_sentValue[kValueCount];
static char g_sentModel[IR_PATH_MAX], g_sentIr[IR_PATH_MAX], g_sentPreset[IR_PATH_MAX];
static bool g_sentEdited = false, g_sentCard = false;
static uint32_t g_lastPollMs = 0;
static uint32_t g_sentPresets = 0;  // presetListHash() of the list the app knows

// Meter frames per second the app asked for (SUBSCRIBE; 0 = none), and when the last one went.
static uint16_t g_meterRate = 0;
static uint32_t g_lastMeterMs = 0;
static bool g_tunerByApp = false;  // the app opened the tuner: closed again when the session ends

static void sendFrame(Msg type, uint8_t flags, uint16_t sequence, const Writer& w) {
  if (!w.ok()) return;  // a reply that did not fit its buffer: a firmware bug, send nothing
  const size_t n = applink::encodeFrame((uint8_t)type, flags, sequence, w.data(), (uint32_t)w.size(), g_txFrame,
                                        sizeof(g_txFrame));
  if (n) Serial.write(g_txFrame, n);
}

static void reply(const applink::FrameHeader& h, const Writer& w) {
  sendFrame((Msg)h.type, applink::kFlagReply, h.sequence, w);
}

static void replyError(const applink::FrameHeader& h, Err code, const char* message) {
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)code);
  w.str(message);
  sendFrame((Msg)h.type, applink::kFlagReply | applink::kFlagError, h.sequence, w);
}

static const char* modelPath() { return ampCurrent() >= 0 ? ampPath(ampCurrent()) : ""; }
static const char* loadedIrPath() { return irCurrent() >= 0 ? irPath(irCurrent()) : ""; }
static const char* recalledPreset() { return presetCurrent() >= 0 ? presetName(presetCurrent()) : ""; }

// Changes when a preset is added, deleted or renamed.
static uint32_t presetListHash() {
  uint32_t h = (uint32_t)presetCount();
  for (int i = 0; i < presetCount(); i++) {
    const char* n = presetName(i);
    h = applink::crc32((const uint8_t*)n, strlen(n)) ^ (h * 31u);
  }
  return h;
}

// The fields of the GET_STATE reply from the model path on, also the STATE_CHANGED event.
static void writeFiles(Writer& w) {
  w.str(modelPath());
  w.str(loadedIrPath());
  w.str(recalledPreset());
  w.u8(presetEdited());
  w.u8(irSdOk());
}

static void rememberFiles() {
  strlcpy(g_sentModel, modelPath(), sizeof(g_sentModel));
  strlcpy(g_sentIr, loadedIrPath(), sizeof(g_sentIr));
  strlcpy(g_sentPreset, recalledPreset(), sizeof(g_sentPreset));
  g_sentEdited = presetEdited();
  g_sentCard = irSdOk();
}

static void writeSchema(Writer& w) {
  w.u16(kValueCount);
  for (int id = 0; id < kValueCount; id++) {
    const ParamDef& d = paramDef(id);
    int lo, hi;
    paramLimits(id, &lo, &hi);
    w.str(d.key);
    w.str(d.name);
    w.str(d.label);
    w.i16((int16_t)lo);
    w.i16((int16_t)hi);
    w.i16(d.def);
    w.u8(d.step);
    w.u8((uint8_t)d.unit);
    w.u8(d.flags);
    // A setting's choices are named for 0 .. its table maximum. The drum styles, kits and
    // songs have no names in the table: the firmware's lists supply them, 0 .. the limit.
    if (id == kDrumStyle || id == kDrumKit || id == kDrumSong) {
      w.u16((uint16_t)(hi + 1));
      for (int c = 0; c <= hi; c++)
        w.str(id == kDrumStyle ? dsp::drumStyle(c).title : id == kDrumKit ? dsp::drumKit(c).name : c ? drumSongTitle(c) : "off");
      continue;
    }
    const int choices = d.choices ? d.max + 1 : 0;
    w.u16((uint16_t)choices);
    for (int c = 0; c < choices; c++) w.str(d.choices[c]);
  }
}

static uint32_t schemaHash() {
  Writer w(g_txPayload, kTxPayload);
  writeSchema(w);
  return applink::crc32(w.data(), w.size());
}

static void rememberTrack();

static void onHello(const applink::FrameHeader& h, Reader& r) {
  const uint16_t appVersion = r.u16();
  char appName[64];
  r.str(appName, sizeof(appName));
  if (!r.ok()) return replyError(h, Err::kBadPayload, "Bad HELLO");
  if (appVersion != applink::kProtocolVersion) return replyError(h, Err::kVersion, "This firmware speaks protocol version 2");

  const uint32_t hash = schemaHash();  // uses the payload buffer: before the reply is built
  Writer w(g_txPayload, kTxPayload);
  w.u16(applink::kProtocolVersion);
  w.str(FIRMWARE_VERSION);
  w.u16((uint16_t)kBlock);
  w.u32((uint32_t)kSampleRate);
  w.u16(AMP_MAX_FILES);
  w.u16(IR_MAX_FILES);
  w.u16(IR_PATH_MAX - 1);
  w.u16(kParamCount);
  w.u16(kValueCount);
  w.u32(hash);
  reply(h, w);

  // A new session: the app asks for the state next, so changes are counted from now.
  g_session = true;
  g_meterRate = 0;
  for (int id = 0; id < kValueCount; id++) g_sentValue[id] = (int16_t)getParam(id);
  rememberFiles();
  g_sentPresets = presetListHash();
  rememberTrack();
  Serial.printf("App connected: %s\n", appName);
}

static void onGetSchema(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  writeSchema(w);
  reply(h, w);
}

static void onGetState(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  w.u16(kValueCount);
  for (int id = 0; id < kValueCount; id++) {
    g_sentValue[id] = (int16_t)getParam(id);
    w.i16(g_sentValue[id]);
  }
  writeFiles(w);
  rememberFiles();
  reply(h, w);
}

static void onSetParam(const applink::FrameHeader& h, Reader& r) {
  const uint16_t id = r.u16();
  const int32_t value = r.i32();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad SET_PARAM");
  if (id >= kValueCount) return replyError(h, Err::kNotFound, "No such setting");
  int lo, hi;
  paramLimits(id, &lo, &hi);
  const int v = value < lo ? lo : (value > hi ? hi : (int)value);
  // Quiet for the saved settings (a dragged knob would flood the shell's output); the few
  // others (the drums' start and stop, the song loop) only have the announcing setter.
  if (id < kParamCount) setParamQuiet(id, v);
  else setParam(id, v);
  const int now = getParam(id);
  g_sentValue[id] = (int16_t)now;  // the reply carries it: no event for the app's own change
  Writer w(g_txPayload, kTxPayload);
  w.u16(id);
  w.i32(now);
  reply(h, w);
}

// ---- Presets (PROTOCOL.md 4.3). The library prints what it did, or why not, to the shell.

static void onPresetList(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)presetCount());
  for (int i = 0; i < presetCount(); i++) {
    w.str(presetName(i));
    w.u16((uint16_t)presetNumber(i));
  }
  g_sentPresets = presetListHash();
  reply(h, w);
}

// The preset with exactly this name, or -1.
static int presetByName(const char* name) {
  for (int i = 0; i < presetCount(); i++)
    if (!strcmp(presetName(i), name)) return i;
  return -1;
}

static void replyEmpty(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  reply(h, w);
}

static void replyCurrentName(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  w.str(recalledPreset());
  reply(h, w);
}

static void onPreset(const applink::FrameHeader& h, Reader& r) {
  const Msg type = (Msg)h.type;
  char name[IR_PATH_MAX] = "";
  if (type != Msg::kPresetSave) r.str(name, sizeof(name));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad preset request");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  switch (type) {
    case Msg::kPresetRecall: {
      const int i = presetByName(name);
      if (i < 0) return replyError(h, Err::kNotFound, "No such preset");
      if (!recallPreset(i)) return replyError(h, Err::kIo, "The preset could not be read");
      return replyEmpty(h);
    }
    case Msg::kPresetSave:
      if (presetCurrent() < 0) return replyError(h, Err::kNotFound, "No preset is recalled; save as a new one");
      if (!savePreset()) return replyError(h, Err::kIo, "The preset could not be written");
      return replyEmpty(h);
    case Msg::kPresetNew:
      if (presetCount() >= 99) return replyError(h, Err::kFull, "All 99 preset numbers are taken");
      if (!newPreset(name)) return replyError(h, Err::kIo, "The preset could not be written");
      return replyCurrentName(h);
    case Msg::kPresetRename:
      if (presetCurrent() < 0) return replyError(h, Err::kNotFound, "No preset is recalled");
      if (!renamePreset(name)) return replyError(h, Err::kBadPayload, "The name is empty or already in use");
      return replyCurrentName(h);
    default: {  // kPresetDelete
      const int i = presetByName(name);
      if (i < 0) return replyError(h, Err::kNotFound, "No such preset");
      if (!deletePreset(i)) return replyError(h, Err::kIo, "The preset could not be deleted");
      return replyEmpty(h);
    }
  }
}

// ---- Amp models and IRs (PROTOCOL.md 4.4)

static uint32_t fileSize(const char* path) {
  File f = SD.open(path, FILE_READ);
  const uint32_t n = f ? (uint32_t)f.size() : 0;
  f.close();
  return n;
}

static void onFileList(const applink::FrameHeader& h, bool models) {
  // The built-in model (listed when the card has none) is not a file: left out.
  const bool card = irSdOk();
  int n = models ? ampCount() : irCount();
  if (models && n == 1 && ampPath(0)[0] != '/') n = 0;
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)n);
  for (int i = 0; i < n; i++) {
    const char* path = models ? ampPath(i) : irPath(i);
    w.str(path);
    w.u32(card ? fileSize(path) : 0);
    w.u32(0);  // modified time: not reported yet
  }
  reply(h, w);
}

static void onFileLoad(const applink::FrameHeader& h, Reader& r, bool model) {
  char path[IR_PATH_MAX];
  r.str(path, sizeof(path));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad load request");
  const int n = model ? ampCount() : irCount();
  int idx = -1;
  for (int i = 0; i < n; i++)
    if (!strcmp(model ? ampPath(i) : irPath(i), path)) idx = i;
  if (idx < 0) return replyError(h, Err::kNotFound, model ? "No such model" : "No such IR");
  if (!(model ? loadAmpIndex(idx) : loadIRIndex(idx)))
    return replyError(h, Err::kIo, model ? "The model could not be loaded (not an A2-Lite model?)" : "The IR could not be loaded");
  replyEmpty(h);
}

static void onModelInfo(const applink::FrameHeader& h) {
  if (ampCurrent() < 0) return replyError(h, Err::kNotFound, "No model is loaded");
  const dsp::NamInfo& m = ampInfo();
  Writer w(g_txPayload, kTxPayload);
  w.str(m.name);
  w.str(m.gearMake);
  w.str(m.gearModel);
  w.str(m.gearType);
  w.str(m.toneType);
  w.f32(m.sampleRate);
  w.f32(m.loudness);
  w.f32(m.inputLevelDbu);
  w.f32(m.outputLevelDbu);
  w.f32(ampCalibrationDb());  // NaN: the calibration is off
  w.f32(ampLevelDb());
  reply(h, w);
}

static void onIrInfo(const applink::FrameHeader& h) {
  if (irCurrent() < 0) return replyError(h, Err::kNotFound, "No IR is loaded");
  const dsp::IRPrepResult& r = irInfo();
  Writer w(g_txPayload, kTxPayload);
  w.u32(r.wav.sampleRate);
  w.u16(r.wav.channels);
  w.u16(r.wav.bitsPerSample);
  w.u32((uint32_t)r.sourceFrames);
  w.u32((uint32_t)r.length);
  w.u8(r.resampled);
  w.u8(r.truncated);
  w.f32(r.gain);
  reply(h, w);
}

// The loaded IR's samples as the chain uses them, as 16-bit numbers with one scale for all:
// enough for a picture of it and its frequency response, in one frame.
static void onIrData(const applink::FrameHeader& h) {
  const float* ir = irSamples();
  if (!ir) return replyError(h, Err::kNotFound, "No IR is loaded");
  const int n = irInfo().length;
  float peak = 0.0f;
  for (int i = 0; i < n; i++) peak = fmaxf(peak, fabsf(ir[i]));
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)n);
  w.f32(peak);
  const float k = peak > 0.0f ? 32767.0f / peak : 0.0f;
  for (int i = 0; i < n; i++) w.u16((uint16_t)(int16_t)lroundf(ir[i] * k));
  reply(h, w);
}

// ---- Files on the SD card (PROTOCOL.md 4.5)

static const char* const kFolders[] = {"/IR/", "/AMP/", "/PRESETS/", "/DRUMS/", "/TRACKS/"};

// A path's longest in the file messages. (The folders but /TRACKS take IR_PATH_MAX at most:
// that is what the unit's lists hold.)
static constexpr int kFilePathMax = 264;

// At or below /TRACKS, where the backing tracks may lie in folders: no part of the path
// is empty or hidden.
static bool inTracks(const char* p) {
  if (strncasecmp(p, "/TRACKS", 7) || (p[7] && p[7] != '/')) return false;
  for (const char* c = p + 7; *c; c++)
    if (c[0] == '/' && (c[1] == '/' || c[1] == '.' || !c[1])) return false;
  return true;
}

// A file the app may write or delete: directly inside one of the folders, or a .wav in the
// root; a backing track may also be in a folder below /TRACKS. No hidden names, no "..".
static bool allowedPath(const char* p) {
  const size_t len = strlen(p);
  if (p[0] != '/' || len >= (size_t)(inTracks(p) ? kFilePathMax : IR_PATH_MAX)) return false;
  for (const char* c = p; *c; c++)
    if ((uint8_t)*c < 0x20 || *c == '\\' || *c == ':') return false;
  const char* name = strrchr(p, '/') + 1;
  if (!name[0] || name[0] == '.' || strstr(p, "..")) return false;
  if (name == p + 1) return len > 5 && !strcasecmp(p + len - 4, ".wav");  // in the root
  if (inTracks(p)) return len > 8;
  for (const char* f : kFolders)
    if (!strncasecmp(p, f, strlen(f)) && name == p + strlen(f)) return true;
  return false;
}

static constexpr uint16_t kPutChunk = 8192;
static constexpr uint32_t kPutTimeoutMs = 10000;
static File g_put;
static char g_putPath[kFilePathMax], g_putTmp[kFilePathMax + 12];
static uint32_t g_putSize = 0, g_putCrcWanted = 0, g_putGot = 0, g_putCrc = 0, g_putLastMs = 0;

static void abandonPut() {
  if (!g_put) return;
  g_put.close();
  SD.remove(g_putTmp);
}

static bool isIn(const char* path, const char* folder) { return !strncasecmp(path, folder, strlen(folder)); }

static void onPutBegin(const applink::FrameHeader& h, Reader& r) {
  char path[kFilePathMax];
  r.str(path, sizeof(path));
  const uint32_t size = r.u32(), crc = r.u32();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FILE_PUT_BEGIN");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  if (!allowedPath(path)) return replyError(h, Err::kBadPath, "Files can only go to /AMP, /IR, /PRESETS, /DRUMS or /TRACKS");
  abandonPut();
  const bool exists = SD.exists(path);
  if (!exists && isIn(path, "/AMP/") && ampCount() >= AMP_MAX_FILES && ampPath(0)[0] == '/')
    return replyError(h, Err::kFull, "The unit's model list is full");
  if (!exists && !isIn(path, "/AMP/") && !isIn(path, "/PRESETS/") && !isIn(path, "/DRUMS/") && !isIn(path, "/TRACKS/") &&
      irCount() >= IR_MAX_FILES)
    return replyError(h, Err::kFull, "The unit's IR list is full");
  // The temporary file: a dot-name beside the target, which the scans skip.
  const int dirLen = (int)(strrchr(path, '/') - path) + 1;
  char dir[kFilePathMax];
  snprintf(dir, sizeof(dir), "%.*s", dirLen > 1 ? dirLen - 1 : 1, path);
  if (dirLen > 1 && !SD.exists(dir) && !SD.mkdir(dir)) return replyError(h, Err::kIo, "Cannot create the folder");
  snprintf(g_putTmp, sizeof(g_putTmp), "%.*s.upload.tmp", dirLen, path);
  SD.remove(g_putTmp);
  g_put = SD.open(g_putTmp, FILE_WRITE);
  if (!g_put) return replyError(h, Err::kIo, "Cannot write to the SD card");
  strlcpy(g_putPath, path, sizeof(g_putPath));
  g_putSize = size, g_putCrcWanted = crc, g_putGot = 0, g_putCrc = 0, g_putLastMs = millis();
  Writer w(g_txPayload, kTxPayload);
  w.u16(kPutChunk);
  reply(h, w);
}

static void onPutData(const applink::FrameHeader& h) {
  if (!g_put) return replyError(h, Err::kBadPayload, "No upload is open");
  if (h.length < 4) return replyError(h, Err::kBadPayload, "Bad FILE_PUT_DATA");
  Reader r(g_parser.payload(), h.length);
  const uint32_t offset = r.u32(), n = h.length - 4;
  const uint8_t* data = g_parser.payload() + 4;
  if (offset != g_putGot || n > kPutChunk || g_putGot + n > g_putSize) {
    abandonPut();
    return replyError(h, Err::kBadPayload, "Upload data out of order or too long");
  }
  if (g_put.write(data, n) != n) {
    abandonPut();
    return replyError(h, Err::kIo, "Writing to the SD card failed (card full?)");
  }
  g_putCrc = applink::crc32(data, n, g_putCrc);
  g_putGot += n;
  g_putLastMs = millis();
  Writer w(g_txPayload, kTxPayload);
  w.u32(g_putGot);
  reply(h, w);
}

static void onPutEnd(const applink::FrameHeader& h) {
  if (!g_put) return replyError(h, Err::kBadPayload, "No upload is open");
  g_put.close();
  if (g_putGot != g_putSize || g_putCrc != g_putCrcWanted) {
    SD.remove(g_putTmp);
    return replyError(h, Err::kChecksum, "The file arrived damaged; nothing was saved");
  }
  const bool track = inTracks(g_putPath);
  if (track) trackLetGo(g_putPath + 8);  // an open track that is being replaced
  if (SD.exists(g_putPath)) SD.remove(g_putPath);
  if (!SD.rename(g_putTmp, g_putPath)) {
    SD.remove(g_putTmp);
    return replyError(h, Err::kIo, "Cannot put the file in place");
  }
  if (isIn(g_putPath, "/AMP/")) ampForgetCache(g_putPath);
  Serial.printf("Received %s (%lu bytes)\n", g_putPath, (unsigned long)g_putSize);
  if (track) trackFilesChanged();  // only the list of tracks: the card is not mounted anew
  else rescanCard();
  replyEmpty(h);
}

static void onFileDelete(const applink::FrameHeader& h, Reader& r) {
  char path[kFilePathMax];
  r.str(path, sizeof(path));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FILE_DELETE");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  if (!allowedPath(path)) return replyError(h, Err::kBadPath, "Only files in /AMP, /IR, /PRESETS, /DRUMS or /TRACKS can be deleted");
  if (!SD.exists(path)) return replyError(h, Err::kNotFound, "No such file");
  const bool track = inTracks(path);
  if (track) {
    File f = SD.open(path, FILE_READ);
    const bool folder = f && f.isDirectory();
    f.close();
    if (folder) return replyError(h, Err::kBadPath, "That is a folder");
    trackLetGo(path + 8);
  }
  if (!SD.remove(path)) return replyError(h, Err::kIo, "The file could not be deleted");
  if (isIn(path, "/AMP/")) ampForgetCache(path);
  Serial.printf("Deleted %s\n", path);
  if (track) {
    char info[kFilePathMax + 8];  // what the unit kept about the track goes with it
    snprintf(info, sizeof(info), "%s.trk", path);
    SD.remove(info);
    trackFilesChanged();
  } else {
    rescanCard();
  }
  replyEmpty(h);
}

// ---- Renaming, and folders: below /TRACKS only (PROTOCOL.md 4.5)

static bool isFolder(const char* path) {
  File f = SD.open(path, FILE_READ);
  const bool folder = f && f.isDirectory();
  f.close();
  return folder;
}

// Makes the folder a path's last part lies in, if it is missing.
static bool makeParent(const char* path) {
  char dir[kFilePathMax];
  const int n = (int)(strrchr(path, '/') - path);
  snprintf(dir, sizeof(dir), "%.*s", n, path);
  return n <= 0 || SD.exists(dir) || SD.mkdir(dir);
}

static void onFileRename(const applink::FrameHeader& h, Reader& r) {
  char from[kFilePathMax], to[kFilePathMax];
  r.str(from, sizeof(from));
  r.str(to, sizeof(to));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FILE_RENAME");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  const size_t n = strlen(from);
  if (!allowedPath(from) || !allowedPath(to) || !inTracks(from) || !inTracks(to))
    return replyError(h, Err::kBadPath, "Only tracks and folders below /TRACKS can be renamed or moved");
  if (!strncasecmp(to, from, n) && to[n] == '/') return replyError(h, Err::kBadPath, "A folder cannot be moved into itself");
  if (!SD.exists(from)) return replyError(h, Err::kNotFound, "No such track or folder");
  // The same name in other letters (the card does not tell upper from lower case) is a
  // new name; anything else that exists is in the way.
  if (strcasecmp(from, to) && SD.exists(to)) return replyError(h, Err::kBadPayload, "There is already something with that name");
  if (!strcmp(from, to)) return replyEmpty(h);
  const bool folder = isFolder(from);
  trackLetGo(from + 8);
  if (!makeParent(to) || !SD.rename(from, to)) return replyError(h, Err::kIo, "It could not be renamed");
  if (!folder) {
    char infoFrom[kFilePathMax + 8], infoTo[kFilePathMax + 8];
    snprintf(infoFrom, sizeof(infoFrom), "%s.trk", from);
    snprintf(infoTo, sizeof(infoTo), "%s.trk", to);
    if (SD.exists(infoFrom)) {
      if (strcasecmp(infoFrom, infoTo)) SD.remove(infoTo);
      SD.rename(infoFrom, infoTo);
    }
  }
  Serial.printf("Renamed %s to %s\n", from, to);
  trackFilesChanged();
  replyEmpty(h);
}

static void onFolderNew(const applink::FrameHeader& h, Reader& r) {
  char path[kFilePathMax];
  r.str(path, sizeof(path));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FOLDER_NEW");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  if (!allowedPath(path) || !inTracks(path)) return replyError(h, Err::kBadPath, "Folders can only be made below /TRACKS");
  if (SD.exists(path)) {
    if (!isFolder(path)) return replyError(h, Err::kBadPayload, "There is a file with that name");
    return replyEmpty(h);
  }
  if (!SD.mkdir(path)) return replyError(h, Err::kIo, "The folder could not be made");
  Serial.printf("New folder %s\n", path);
  trackFilesChanged();
  replyEmpty(h);
}

// Removes a folder with everything in it. The folder is opened anew for each entry (the
// first that is left), since entries go while it is read. `path` has room for the names
// below it. False as soon as something cannot be removed.
static bool removeTree(char* path, size_t cap, uint16_t* files, int depth) {
  for (;;) {
    File d = SD.open(path, FILE_READ);
    if (!d) return false;
    File e = d.openNextFile();
    if (!e) {
      d.close();
      break;
    }
    const size_t len = strlen(path);
    const bool folder = e.isDirectory();
    const char* name = e.name();
    const bool counted = !folder && name[0] != '.' && strcasecmp(name + (strlen(name) > 4 ? strlen(name) - 4 : 0), ".trk");
    const int n = snprintf(path + len, cap - len, "/%s", name);
    e.close();
    d.close();
    bool gone = false;
    if (n > 0 && (size_t)n < cap - len) gone = folder ? (depth < 12 && removeTree(path, cap, files, depth + 1)) : SD.remove(path);
    path[len] = 0;
    if (!gone) return false;
    if (counted) (*files)++;
  }
  return SD.rmdir(path);
}

static void onFolderDelete(const applink::FrameHeader& h, Reader& r) {
  EXTMEM static char path[kFilePathMax + 2 * dsp::TrackIndex::kNameMax];  // with room for the names below it
  r.str(path, kFilePathMax);
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FOLDER_DELETE");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  if (!allowedPath(path) || !inTracks(path)) return replyError(h, Err::kBadPath, "Only folders below /TRACKS can be removed");
  if (!SD.exists(path) || !isFolder(path)) return replyError(h, Err::kNotFound, "No such folder");
  trackLetGo(path + 8);
  uint16_t files = 0;
  const bool ok = removeTree(path, sizeof(path), &files, 0);
  Serial.printf("Removed folder %s (%u files)%s\n", path, files, ok ? "" : ": not all of it");
  trackFilesChanged();
  if (!ok) return replyError(h, Err::kIo, "Not everything in the folder could be removed");
  Writer w(g_txPayload, kTxPayload);
  w.u16(files);
  reply(h, w);
}

// CARD_INFO: the card's size and free space. Counting the free space reads the card's whole
// allocation table, which takes a moment on a large card (the audio keeps running).
static void onCardInfo(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  const bool card = irSdOk();
  const uint64_t total = card ? SD.totalSize() : 0, used = card ? SD.usedSize() : 0;
  w.u8(card);
  w.u32((uint32_t)(total / 1024));
  w.u32((uint32_t)((total > used ? total - used : 0) / 1024));
  reply(h, w);
}

// DIR_LIST: the files of one of the folders the app may see, or of a folder below /TRACKS
// (no hidden names, no subfolders' contents).
static void onDirList(const applink::FrameHeader& h, Reader& r) {
  char folder[kFilePathMax];
  r.str(folder, sizeof(folder));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad DIR_LIST");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  bool allowed = !strcmp(folder, "/") || (inTracks(folder) && !strstr(folder, ".."));
  for (const char* f : kFolders)
    if (!strncasecmp(folder, f, strlen(f) - 1) && strlen(folder) == strlen(f) - 1) allowed = true;
  if (!allowed) return replyError(h, Err::kBadPath, "Only /, /AMP, /IR, /PRESETS, /DRUMS, /TRACKS and the folders below /TRACKS can be listed");
  File d = SD.open(folder);
  Writer w(g_txPayload, kTxPayload);
  if (!d || !d.isDirectory()) {  // a folder that does not exist yet: empty
    w.u16(0);
    return reply(h, w);
  }
  // The count goes first: the entries are written to a second writer behind its place.
  uint16_t count = 0;
  Writer entries(g_txPayload + 2, kTxPayload - 2);
  for (;;) {
    File e = d.openNextFile();
    if (!e) break;
    const char* name = e.name();
    if (name[0] != '.' && entries.size() + strlen(name) + 16 < kTxPayload - 2) {
      entries.str(name);
      entries.u32(e.isDirectory() ? 0 : (uint32_t)e.size());
      entries.u32(0);  // modified time: not reported yet
      entries.u8(e.isDirectory());
      count++;
    }
    e.close();
  }
  d.close();
  g_txPayload[0] = (uint8_t)count;
  g_txPayload[1] = (uint8_t)(count >> 8);
  const size_t n = applink::encodeFrame((uint8_t)h.type, applink::kFlagReply, h.sequence, g_txPayload,
                                        (uint32_t)(entries.size() + 2), g_txFrame, sizeof(g_txFrame));
  if (entries.ok() && n) Serial.write(g_txFrame, n);
}

// FILE_GET: the file goes out as FILE_DATA replies, one chunk per pass of loop(), so the
// shell, MIDI and the meters keep running during a long download.
static constexpr uint32_t kGetChunk = 4096;
static File g_get;
static uint32_t g_getSize = 0, g_getSent = 0;
static uint16_t g_getSequence = 0;

static void onFileGet(const applink::FrameHeader& h, Reader& r) {
  char path[kFilePathMax];
  r.str(path, sizeof(path));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad FILE_GET");
  if (!irSdOk()) return replyError(h, Err::kNoCard, "No SD card");
  if (!allowedPath(path)) return replyError(h, Err::kBadPath, "Only files in /AMP, /IR, /PRESETS, /DRUMS or /TRACKS can be read");
  if (g_get) g_get.close();
  g_get = SD.open(path, FILE_READ);
  if (!g_get || g_get.isDirectory()) {
    g_get.close();
    return replyError(h, Err::kNotFound, "No such file");
  }
  g_getSize = (uint32_t)g_get.size();
  g_getSent = 0;
  g_getSequence = h.sequence;
}

// Sends the next chunk of a download, if one is running.
static void serviceFileGet(uint32_t now) {
  if (!g_get) return;
  const uint32_t want = g_getSize - g_getSent < kGetChunk ? g_getSize - g_getSent : kGetChunk;
  Writer w(g_txPayload, kTxPayload);
  w.u32(g_getSent);
  w.u32(g_getSize);
  const int got = want ? g_get.read(g_txPayload + 8, want) : 0;
  if (got != (int)want) {  // the card failed: end the download with an error
    g_get.close();
    Writer e(g_txPayload, kTxPayload);
    e.u16((uint16_t)Err::kIo);
    e.str("Reading the SD card failed");
    sendFrame(Msg::kFileGet, applink::kFlagReply | applink::kFlagError, g_getSequence, e);
    return;
  }
  const size_t n = applink::encodeFrame((uint8_t)Msg::kFileData, applink::kFlagReply, g_getSequence, g_txPayload, 8 + want,
                                        g_txFrame, sizeof(g_txFrame));
  if (n) Serial.write(g_txFrame, n);
  g_getSent += want;
  // A download is the session being alive. (`now`, not millis(): the caller compares its own
  // `now` with this, and a later time would look like a very old one.)
  g_lastFrameMs = now;
  if (g_getSent >= g_getSize) g_get.close();
}

static void onRescan(const applink::FrameHeader& h) {
  rescanCard();
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)irCount());
  w.u16((uint16_t)(ampCount() == 1 && ampPath(0)[0] != '/' ? 0 : ampCount()));
  w.u16((uint16_t)presetCount());
  w.u16((uint16_t)drumSongTotal());
  reply(h, w);
}

// ---- The backing track (PROTOCOL.md 4.8)

// The state. Without the place and the gaps (which change all the time while a track
// plays) it is the "shape" whose change is reported at once.
static void writeTrackState(Writer& w, bool withPlace) {
  const bool open = trackOpen();
  float from = 0.0f, to = 0.0f;
  uint8_t loop = 0;
  if (open && trackLoopPoints(&from, &to)) {
    loop = trackPlayerLooping() ? 2 : 1;
  } else if (open && trackLoopStartMarked()) {
    loop = 3;
    from = trackLoopStartMark();
    to = 0.0f;
  }
  w.str(trackOpenPath());
  w.u8((uint8_t)trackState());
  w.f32(open && withPlace ? trackPosition() : 0.0f);
  w.f32(open ? trackLength() : 0.0f);
  w.u16((uint16_t)trackTempoPercent());
  w.f32(trackBpm());
  w.u8(open && trackBpm() > 0.0f && trackBpmSure());
  w.i16((int16_t)trackScanPercent());
  w.f32(trackBpm() > 0.0f ? trackFirstBeat() : 0.0f);
  w.u8((uint8_t)trackBeatsInBar());
  w.u8(trackSnapOn());
  w.u8(loop);
  w.f32(from);
  w.f32(to);
  w.u32(withPlace ? trackGaps() : 0);
  w.u8((uint8_t)trackFileKind());
  w.u32(trackFileRate());
  w.u8((uint8_t)trackFileChannels());
  w.u16((uint16_t)trackFileKbps());
  w.u16((uint16_t)trackIndex().files());
  w.u8(trackReadingCard());
  w.u32(trackListNumber());
}

// What the app was last told about the track.
static uint32_t g_sentTrackShape = 0, g_sentTrackAll = 0, g_sentTrackMs = 0;

static uint32_t trackStateCrc(bool withPlace) {
  uint8_t buf[dsp::TrackIndex::kPathMax + 80];
  Writer w(buf, sizeof(buf));
  writeTrackState(w, withPlace);
  return applink::crc32(w.data(), w.size());
}

static void rememberTrack() {
  g_sentTrackShape = trackStateCrc(false);
  g_sentTrackAll = trackStateCrc(true);
  g_sentTrackMs = millis();
}

static void replyTrackState(const applink::FrameHeader& h) {
  Writer w(g_txPayload, kTxPayload);
  writeTrackState(w, true);
  rememberTrack();  // the reply carries it: no event for the app's own change
  reply(h, w);
}

// A change is reported within a pass or two; the place while it moves, four times a second.
static void reportTrack(uint32_t now) {
  const uint32_t shape = trackStateCrc(false);
  if (shape == g_sentTrackShape) {
    if (now - g_sentTrackMs < 250 || trackStateCrc(true) == g_sentTrackAll) return;
  }
  Writer w(g_txPayload, kTxPayload);
  writeTrackState(w, true);
  rememberTrack();
  sendFrame(Msg::kTrackChanged, applink::kFlagEvent, 0, w);
}

// TRACK_LIST: the rows of one folder of the unit's list, from a row on, as many as fit.
static void onTrackList(const applink::FrameHeader& h, Reader& r) {
  char folder[dsp::TrackIndex::kPathMax];
  r.str(folder, sizeof(folder));
  const int first = r.u16();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad TRACK_LIST");
  const dsp::TrackIndex& ix = trackIndex();
  const int id = folder[0] ? ix.findPath(folder) : 0;
  if (id < 0 || !ix.isFolder(id)) return replyError(h, Err::kNotFound, "No such folder");
  const int rows = ix.rows(id), from = first < rows ? first : rows;
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)rows);
  w.u16((uint16_t)from);
  const size_t countAt = w.size();
  w.u16(0);
  uint16_t count = 0;
  for (int i = from; i < rows; i++) {
    const int row = ix.row(id, i);
    const char* name = ix.name(row);
    if (w.size() + strlen(name) + 8 > kTxPayload) break;
    w.str(name);
    w.u8(ix.isFolder(row));
    w.u16((uint16_t)ix.rows(row));
    count++;
  }
  g_txPayload[countAt] = (uint8_t)count;
  g_txPayload[countAt + 1] = (uint8_t)(count >> 8);
  reply(h, w);
}

// TRACK_FIND: the search over all folders, as paths.
static void onTrackFind(const applink::FrameHeader& h, Reader& r) {
  EXTMEM static uint16_t hits[dsp::TrackBrowser::kMaxHits];
  char text[64], path[dsp::TrackIndex::kPathMax];
  r.str(text, sizeof(text));
  const int first = r.u16();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad TRACK_FIND");
  const dsp::TrackIndex& ix = trackIndex();
  bool more = false;
  const int n = ix.search(text, hits, dsp::TrackBrowser::kMaxHits, &more), from = first < n ? first : n;
  Writer w(g_txPayload, kTxPayload);
  w.u16((uint16_t)n);
  w.u8(more);
  w.u16((uint16_t)from);
  const size_t countAt = w.size();
  w.u16(0);
  uint16_t count = 0;
  for (int i = from; i < n; i++) {
    if (!ix.pathOf(hits[i], path, sizeof(path))) path[0] = 0;
    if (w.size() + strlen(path) + 8 > kTxPayload) break;
    w.str(path);
    count++;
  }
  g_txPayload[countAt] = (uint8_t)count;
  g_txPayload[countAt + 1] = (uint8_t)(count >> 8);
  reply(h, w);
}

static void onTrackOpen(const applink::FrameHeader& h, Reader& r) {
  char path[dsp::TrackIndex::kPathMax];
  r.str(path, sizeof(path));
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad TRACK_OPEN");
  if (!path[0]) {
    selectTrack(-1);
    return replyTrackState(h);
  }
  const int id = trackIndex().findPath(path);
  if (id <= 0 || trackIndex().isFolder(id)) return replyError(h, Err::kNotFound, "No such track");
  if (!selectTrack(id)) return replyError(h, Err::kIo, "The track could not be opened (not a WAV or MP3 the unit plays?)");
  replyTrackState(h);
}

static void onTrackCommand(const applink::FrameHeader& h, Reader& r) {
  const uint8_t command = r.u8();
  const float a = r.f32(), b = r.f32(), c = r.f32();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad TRACK_COMMAND");
  if (!trackOpen()) return replyError(h, Err::kNotFound, "No track is open");
  const char* refused = nullptr;
  switch (command) {
    case 1: trackPlay(); break;
    case 2: trackPause(); break;
    case 3: trackStop(); break;
    case 4: trackSeek(a); break;
    case 5:
      if (!(a >= 50.0f && a <= 150.0f)) refused = "The tempo is 50 to 150 percent";
      else trackTempo((int)lroundf(a));
      break;
    case 6:
      if (!(a >= 30.0f && a <= 300.0f)) refused = "A tempo is 30 to 300 BPM";
      else trackSetBpm(a);
      break;
    case 7:
    case 8:
      if (!(trackBpm() > 0.0f)) refused = "The track's tempo is not known";
      else trackScaleBpm(command == 8);
      break;
    case 9:
      if (!(a >= 1.0f && a <= 16.0f)) refused = "A bar has 1 to 16 beats";
      else trackBeatsPerBar((int)lroundf(a));
      break;
    case 10: trackSnap(a != 0.0f); break;
    case 11: trackRescan(); break;
    case 12: trackLoopMark(false); break;
    case 13: trackLoopMark(true); break;
    case 14:
      if (!trackLoop(a, b)) refused = "No such section: it must lie in the track and be 0.25 s or longer";
      break;
    case 15: trackLoopOff(); break;
    case 16:
      if (!trackLoopOn()) refused = "The track has no loop points";
      break;
    case 17: trackLoopClear(); break;
    case 18:
      if (!(a >= 1.0f && a < 30000.0f && b >= 1.0f && b <= 16.0f && c >= 1.0f && c <= 1000.0f) ||
          !trackLoopFrom((int)lroundf(a), (int)lroundf(b), (int)lroundf(c)))
        refused = "No such bars in the track (its tempo must be known)";
      break;
    default: refused = "No such track command"; break;
  }
  if (refused) return replyError(h, Err::kBadPayload, refused);
  replyTrackState(h);
}

// ---- The tuner (PROTOCOL.md 4.6)

static void onTunerOpen(const applink::FrameHeader& h, Reader& r) {
  const bool open = r.u8();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad TUNER_OPEN");
  tunerOpen(open);
  g_tunerByApp = open;
  replyEmpty(h);
}

static void sendTuner() {
  const TunerReading& t = tunerReading();
  Writer w(g_txPayload, kTxPayload);
  w.u8(t.valid);
  w.f32(t.valid ? t.hz : 0.0f);
  w.u8(t.valid ? (uint8_t)t.note.index : 0);
  w.i16(t.valid ? (int16_t)t.note.octave : 0);
  w.f32(t.valid ? t.note.cents : 0.0f);
  sendFrame(Msg::kTuner, applink::kFlagEvent, 0, w);
}

static void onSubscribe(const applink::FrameHeader& h, Reader& r) {
  const uint16_t rate = r.u16();
  if (!r.done()) return replyError(h, Err::kBadPayload, "Bad SUBSCRIBE");
  g_meterRate = rate > 60 ? 60 : rate;
  g_lastMeterMs = millis();
  meterInputPeak(kMeterApp);  // the first frame's peaks start now
  meterOutputPeak(kMeterApp);
  for (int stage = 0; stage < AudioChain::kStageCount; stage++) meterStagePeak(kMeterApp, stage);
  chain.resetCpuMax();
  Writer w(g_txPayload, kTxPayload);
  w.u16(g_meterRate);
  reply(h, w);
}

// One METERS event (PROTOCOL.md 4.6). The peaks and the CPU maximum are "since the last
// frame": they are the same counters the shell's s and m read, so while the app shows its
// meters those two see only what happened since the app's last frame.
static void sendMeters() {
  const i2s32::Stats drv = i2s32::stats();
  Writer w(g_txPayload, kTxPayload);
  w.f32(meterInputPeak(kMeterApp));
  w.f32(meterOutputPeak(kMeterApp));
  w.f32(getParam(kGateOn) ? chain.gate().gain() : 1.0f);
  w.u32(chain.clipCount());
  w.f32(chain.cpuPercent());
  w.f32(chain.cpuPercentMax());
  w.f32(chain.ampCpuPercent());
  w.f32(chain.delayCpuPercent());
  w.f32(chain.reverbCpuPercent());
  w.f32(chain.drumCpuPercent());
  w.u32(drv.overruns);
  w.u32(drv.faults);
  w.u8((chain.amp().active() ? 1 : 0) | (chain.delayActive() ? 2 : 0) | (chain.reverbActive() ? 4 : 0) |
       (chain.drums().playing() ? 8 : 0) | (i2s32::faulted() ? 16 : 0) | (chain.tunerOn() ? 32 : 0));
  for (int stage = 0; stage < AudioChain::kStageCount; stage++) w.f32(meterStagePeak(kMeterApp, stage));
  w.f32(meterLimiterGain(kMeterApp));
  chain.resetCpuMax();
  sendFrame(Msg::kMeters, applink::kFlagEvent, 0, w);
}

static void handleFrame() {
  const applink::FrameHeader& h = g_parser.header();
  if (h.flags) return;  // only requests come from the app
  g_lastFrameMs = millis();
  Reader r(g_parser.payload(), h.length);
  const Msg type = (Msg)h.type;
  if (type == Msg::kHello) return onHello(h, r);
  if (!g_session) return replyError(h, Err::kBadPayload, "Say HELLO first");
  switch (type) {
    case Msg::kGetSchema: onGetSchema(h); break;
    case Msg::kGetState: onGetState(h); break;
    case Msg::kSetParam: onSetParam(h, r); break;
    case Msg::kSubscribe: onSubscribe(h, r); break;
    case Msg::kPresetList: onPresetList(h); break;
    case Msg::kModelList: onFileList(h, true); break;
    case Msg::kIrList: onFileList(h, false); break;
    case Msg::kModelLoad: onFileLoad(h, r, true); break;
    case Msg::kIrLoad: onFileLoad(h, r, false); break;
    case Msg::kModelInfo: onModelInfo(h); break;
    case Msg::kIrInfo: onIrInfo(h); break;
    case Msg::kIrData: onIrData(h); break;
    case Msg::kCardInfo: onCardInfo(h); break;
    case Msg::kFilePutBegin: onPutBegin(h, r); break;
    case Msg::kFilePutData: onPutData(h); break;
    case Msg::kFilePutEnd: onPutEnd(h); break;
    case Msg::kFileDelete: onFileDelete(h, r); break;
    case Msg::kFileRename: onFileRename(h, r); break;
    case Msg::kFolderNew: onFolderNew(h, r); break;
    case Msg::kFolderDelete: onFolderDelete(h, r); break;
    case Msg::kTrackList: onTrackList(h, r); break;
    case Msg::kTrackFind: onTrackFind(h, r); break;
    case Msg::kTrackOpen: onTrackOpen(h, r); break;
    case Msg::kTrackCommand: onTrackCommand(h, r); break;
    case Msg::kTrackState: replyTrackState(h); break;
    case Msg::kRescan: onRescan(h); break;
    case Msg::kDirList: onDirList(h, r); break;
    case Msg::kFileGet: onFileGet(h, r); break;
    case Msg::kTunerOpen: onTunerOpen(h, r); break;
    case Msg::kPresetRecall:
    case Msg::kPresetSave:
    case Msg::kPresetNew:
    case Msg::kPresetRename:
    case Msg::kPresetDelete: onPreset(h, r); break;
    default: replyError(h, Err::kUnknownMessage, "Not supported by this firmware"); break;
  }
  // The time a request took (a folder being removed, an MP3 being opened) is not the
  // app's silence.
  g_lastFrameMs = millis();
}

bool appLinkFeed(uint8_t byte) {
  using Result = applink::FrameParser::Result;
  const Result res = g_parser.feed(byte);
  if (res == Result::kText) return false;
  g_lastByteMs = millis();
  if (res == Result::kFrame) handleFrame();
  return true;
}

// Sends an event for everything that differs from what the app was last told.
static void reportChanges() {
  for (int id = 0; id < kValueCount; id++) {
    const int16_t v = (int16_t)getParam(id);
    if (v == g_sentValue[id]) continue;
    g_sentValue[id] = v;
    Writer w(g_txPayload, kTxPayload);
    w.u16((uint16_t)id);
    w.i32(v);
    sendFrame(Msg::kParamChanged, applink::kFlagEvent, 0, w);
  }
  if (strcmp(g_sentModel, modelPath()) || strcmp(g_sentIr, loadedIrPath()) || strcmp(g_sentPreset, recalledPreset()) ||
      g_sentEdited != presetEdited() || g_sentCard != irSdOk()) {
    Writer w(g_txPayload, kTxPayload);
    writeFiles(w);
    rememberFiles();
    sendFrame(Msg::kStateChanged, applink::kFlagEvent, 0, w);
  }
  const uint32_t presets = presetListHash();
  if (presets != g_sentPresets) {
    g_sentPresets = presets;
    Writer w(g_txPayload, kTxPayload);
    sendFrame(Msg::kPresetsChanged, applink::kFlagEvent, 0, w);
  }
  reportTrack(millis());
}

void serviceAppLink() {
  const uint32_t now = millis();
  // A frame that stopped half way, or the pause after a bad one (PROTOCOL.md 2.3).
  if (g_parser.busy() && now - g_lastByteMs >= kLineIdleMs) g_parser.idle();
  if (g_put && now - g_putLastMs >= kPutTimeoutMs) abandonPut();  // PROTOCOL.md 4.5
  if (!g_session) return;
  serviceFileGet(now);
  if (now - g_lastFrameMs >= kSessionTimeoutMs) {
    g_session = false;
    if (g_get) g_get.close();
    if (g_tunerByApp && chain.tunerOn()) tunerOpen(false);  // or the guitar would stay muted
    g_tunerByApp = false;
    Serial.println("App disconnected.");
    return;
  }
  if (now - g_lastPollMs >= kChangePollMs) {
    g_lastPollMs = now;
    reportChanges();
  }
  if (chain.tunerOn() && serviceTuner(now)) sendTuner();
  if (g_meterRate && now - g_lastMeterMs >= 1000u / g_meterRate) {
    g_lastMeterMs = now;
    sendMeters();
  }
}
