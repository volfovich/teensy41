// TrackInfo.h - what is kept about a backing track in a small text file beside it
// ("song.mp3.trk"): what the scan found (tempo, where the beats are) and what the player
// was last set to for this track (tempo, loop, where bar 1 is). Also the beat grid's
// arithmetic.
//
// The file, one "key value" line each, any order; unknown lines are skipped:
//
//   track 1            the format's version
//   size 2772705       the audio file's size in bytes: another size means another file
//   scanned 1          the scan has run (0 or missing: it runs again)
//   bpm 120.0003       0: no beat found
//   beat 0.5027        the first beat, seconds
//   confidence 0.97
//   beats 4            beats in a bar
//   bar 2              the beat (counted from the first, 0) that starts bar 1; missing: not set
//   tempo 0.8000       the tempo the track was last played at (1 = its own)
//   loop 12.000 20.000 the loop, seconds (missing: none)
//   snap 1             loop points go to the nearest beat or bar line
//
// Portable; background only.
#pragma once

#include <stdint.h>

namespace dsp {

struct TrackInfo {
  uint32_t fileBytes = 0;
  bool scanned = false;
  float bpm = 0.0f;         // 0: none
  float firstBeat = 0.0f;   // seconds
  float confidence = 0.0f;
  int beatsPerBar = 4;
  bool barSet = false;      // the user has said where bar 1 is
  int barBeat = 0;          // the beat that starts bar 1 (0 = the first beat)
  float tempo = 1.0f;
  float loopFrom = 0.0f, loopTo = 0.0f;  // seconds; loopTo 0: none
  bool snap = true;

  bool hasGrid() const { return bpm > 0.0f; }
  double period() const { return 60.0 / (double)bpm; }
  // The time of beat k (k may be negative: before the first beat).
  double beatTime(long k) const { return (double)firstBeat + (double)k * period(); }
  // The beat at or before t.
  long beatAt(double t) const;
  // The beat nearest to t.
  long nearestBeat(double t) const;
  // The first beat of the bar nearest to t (a bar line).
  long nearestBarLine(double t) const;
  // The first beat of bar n (1 = bar 1).
  long barLine(int bar) const { return (long)barBeat + (long)(bar - 1) * beatsPerBar; }
  // Where beat k is: *bar (1 = bar 1; 0 and below: before it) and *beat (1 .. beatsPerBar).
  void barAndBeat(long k, int* bar, int* beat) const;
  // Says "bar 1 is at t". If t lies nearer to the middle between two beats than to a
  // beat, the grid is taken to be half a beat out and is moved by that much first.
  void setBarOneAt(double t);
};

// Reads the file's text (zero-terminated). False if it is not a track file of this version.
bool parseTrackInfo(const char* text, TrackInfo& info);
// Writes the file's text; returns its length (always below n).
int formatTrackInfo(const TrackInfo& info, char* buf, int n);

}  // namespace dsp
