#pragma once
// Message and error numbers of the desktop app's link protocol (protocol/PROTOCOL.md,
// sections 4 and 5). Keep in step with app/crates/namforge-link/src/lib.rs.

#include <cstdint>

namespace applink {

constexpr uint16_t kProtocolVersion = 2;

enum class Msg : uint8_t {
    // Session
    kHello = 0x01,
    kGetSchema = 0x02,
    kGetState = 0x03,
    // Settings
    kSetParam = 0x10,
    kParamChanged = 0x11,   // event
    kStateChanged = 0x12,   // event: the loaded model, IR or preset, or the edited mark, changed
    // Presets
    kPresetList = 0x20,
    kPresetRecall = 0x21,
    kPresetSave = 0x22,
    kPresetNew = 0x23,
    kPresetRename = 0x24,
    kPresetDelete = 0x25,
    kPresetsChanged = 0x26, // event: the list of presets changed (ask for it again)
    // Amp models and IRs
    kModelList = 0x30,
    kModelLoad = 0x31,
    kModelInfo = 0x32,
    kIrList = 0x38,
    kIrLoad = 0x39,
    kIrInfo = 0x3A,
    kIrData = 0x3B,
    // Files on the SD card
    kFilePutBegin = 0x40,
    kFilePutData = 0x41,
    kFilePutEnd = 0x42,
    kFileGet = 0x43,
    kFileData = 0x44,       // reply chunks to kFileGet
    kFileDelete = 0x45,
    kFileRename = 0x46,
    kDirList = 0x47,
    kCardInfo = 0x48,
    kRescan = 0x49,
    kFolderNew = 0x4A,      // below /TRACKS
    kFolderDelete = 0x4B,   // below /TRACKS, with everything in it
    // Live data
    kSubscribe = 0x50,
    kMeters = 0x51,         // event
    kTunerOpen = 0x52,
    kTuner = 0x53,          // event
    // Other
    kLog = 0x60,            // event
    // Backing track
    kTrackList = 0x70,
    kTrackFind = 0x71,
    kTrackOpen = 0x72,
    kTrackCommand = 0x73,
    kTrackState = 0x74,
    kTrackChanged = 0x75,   // event
};

// The code in an error reply's payload.
enum class Err : uint16_t {
    kUnknownMessage = 1,    // a message type this firmware does not know
    kBadPayload = 2,        // the payload is too short, too long or has a bad value
    kNotFound = 3,          // no such preset, model, IR or file
    kNoCard = 4,            // the SD card is missing
    kBadPath = 5,           // a path outside the allowed folders, or too long
    kBusy = 6,              // a load or a transfer is already running
    kIo = 7,                // the SD card reported an error
    kChecksum = 8,          // an uploaded file's size or CRC did not match
    kFull = 9,              // the list is at its limit, or the card is full
    kVersion = 10,          // the app's protocol version is not supported
};

} // namespace applink
