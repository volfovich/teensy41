// TrackInfo.h - what is kept about a backing track in a small text file beside it
// ("song.mp3.trk"): what the scan found (tempo, where the beats are) and what the player
// was last set to for this track (tempo, the loop's points and whether it is on). Also
// the beat grid's arithmetic.
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
//   tempo 0.8000       the tempo the track was last played at (1 = its own)
//   loop 12.000 20.000 the loop's points, seconds (missing: none)
//   looping 0          the loop is switched off, its points are kept (missing: on)
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
  float tempo = 1.0f;
  float loopFrom = 0.0f, loopTo = 0.0f;  // the loop's points, seconds; loopTo 0: none
  bool looping = true;      // with points: the loop is on (off: the points are kept for later)
  bool snap = true;

  bool hasGrid() const { return bpm > 0.0f; }
  double period() const { return 60.0 / (double)bpm; }
  // The time of beat k (k may be negative: before the first beat).
  double beatTime(long k) const { return (double)firstBeat + (double)k * period(); }
  // The beat at or before t.
  long beatAt(double t) const;
  // The beat nearest to t.
  long nearestBeat(double t) const;
  // The first beat of bar n (1 = bar 1).
  long barLine(int bar) const { return (long)(bar - 1) * beatsPerBar; }
  // Where beat k is: *bar (1 = the bar that starts with the first beat; 0 and below: before
  // it) and *beat (1 .. beatsPerBar).
  void barAndBeat(long k, int* bar, int* beat) const;
};

// Reads the file's text (zero-terminated). False if it is not a track file of this version.
bool parseTrackInfo(const char* text, TrackInfo& info);
// Writes the file's text; returns its length (always below n).
int formatTrackInfo(const TrackInfo& info, char* buf, int n);

}  // namespace dsp
