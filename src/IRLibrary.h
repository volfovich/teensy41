// IRLibrary.h - the list of .wav IRs on the SD card and loading them into the
// chain (resampled, trimmed and normalized by dsp::prepareIR).
// Ported from teensy-ir-loader (src/IRLibrary.h) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#pragma once

// (Re)mounts the SD card and rebuilds the sorted file list.
void scanCard();

// Loads file idx (0-based) and, with save, remembers it in the settings (the
// IR loaded at startup). False on failure.
// Rescans the card while the sound keeps playing: the loaded IR and model stay loaded and
// stay marked in the new lists (unless their files have gone). For after files were added
// or deleted.
void rescanCard();

bool loadIRIndex(int idx, bool save = true);
// Remembers the IR playing now in the settings.
void saveCurrentIR();
// The index of the IR in the settings, or -1 (none, or not on the card).
int irSaved();
// Loads the next (+1) or previous (-1) IR, skipping files that fail to load.
void loadRelative(int step);
// Loads the IR saved in the settings, or the first one that loads.
void loadStartupIR();

bool irSdOk();
int irCount();
int irCurrent();  // -1 = none loaded (passthrough)
const char* irPath(int idx);
