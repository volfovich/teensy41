// AppLink.cpp - see AppLink.h. Milestone A1 of APP.md: the session, the settings table, the
// state, setting a value, and change events.
#include "AppLink.h"

#include <Arduino.h>
#include <applink/Frame.h>
#include <applink/Messages.h>
#include <applink/Wire.h>

#include "AmpLibrary.h"
#include "IRLibrary.h"
#include "Params.h"
#include "PresetLibrary.h"
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
    // A setting's choices are named for 0 .. its table maximum.
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

static void onHello(const applink::FrameHeader& h, Reader& r) {
  const uint16_t appVersion = r.u16();
  char appName[64];
  r.str(appName, sizeof(appName));
  if (!r.ok()) return replyError(h, Err::kBadPayload, "Bad HELLO");
  if (appVersion < 1) return replyError(h, Err::kVersion, "This firmware speaks protocol version 1");

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
  for (int id = 0; id < kValueCount; id++) g_sentValue[id] = (int16_t)getParam(id);
  rememberFiles();
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
    default: replyError(h, Err::kUnknownMessage, "Not supported by this firmware"); break;
  }
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
}

void serviceAppLink() {
  const uint32_t now = millis();
  // A frame that stopped half way, or the pause after a bad one (PROTOCOL.md 2.3).
  if (g_parser.busy() && now - g_lastByteMs >= kLineIdleMs) g_parser.idle();
  if (!g_session) return;
  if (now - g_lastFrameMs >= kSessionTimeoutMs) {
    g_session = false;
    Serial.println("App disconnected.");
    return;
  }
  if (now - g_lastPollMs >= kChangePollMs) {
    g_lastPollMs = now;
    reportChanges();
  }
}

bool appLinkActive() { return g_session; }
