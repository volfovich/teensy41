// MidiControl.h - control over USB MIDI (the Teensy shows up as a MIDI device).
//
//   Program Change n   recalls preset n + 1 (by the number in its name, "03 Lead" = program 2;
//                      if no name has that number, the n-th preset in the list)
//   Control Change     sets a setting, see the table in MidiControl.cpp and docs/CONTROL.md
//
// Any channel. Call serviceMidi() from loop().
#pragma once

void serviceMidi();
// Prints the mapping (the serial command "midi").
void printMidiMap();
