// TrackLibrary.h - the backing tracks: the audio files in /TRACKS on the SD card and in
// the folders below it, as a list that can be gone through folder by folder and searched
// (dsp/player/TrackIndex.h), and playing one (play, pause, stop, seek, looping a section).
// The list is read from the card a little at a time after the card is mounted, so the
// unit does not wait for it; what has been read so far can be used meanwhile.
//
// The file is read in the background, ahead of time, into a queue in the PSRAM that the
// audio interrupt plays from (dsp/player/TrackPlayer.h): serviceTrack() must be called
// from loop(). The track is added to the output beside the drums, after the output gain
// and the effects and before the master volume; its level is the setting "track vol".
//
// Each track is scanned once for its tempo and beats (dsp/player/BeatScan.h), while it is
// open and not playing; what was found, and the tempo and loop the track was left
// with (its points also while the loop is switched off), are kept in a small file beside
// it ("song.mp3.trk").
//
// WAV files (any bit depth, mono or stereo) and MP3 files. A file that is not at 48 kHz is
// resampled while it plays (dsp/player/ResampledSource.h).
#pragma once

#include <stdint.h>

namespace dsp {
class TrackIndex;
class TrackBrowser;
}  // namespace dsp

// Hands the queue to the chain. Call once, before the audio starts.
void trackBegin();
// Starts the list anew (call after the card is mounted); serviceTrack() reads it in. A
// track that was open stays open, and goes on playing, if its file is still there.
void scanTracks();
// Lets go of the open file before the card is mounted again (scanTracks() takes it up).
void trackCardGoing();
// Keeps the queue filled and reads the card into the list; call from loop().
void serviceTrack();

// The list, and a place in it: the shell's or the touch screen's. Tracks and folders are
// known by their number in the list (id), which holds until the card is read again.
const dsp::TrackIndex& trackIndex();
dsp::TrackBrowser& trackBrowser(bool screen);
bool trackReadingCard();  // the list is not complete yet
bool trackOpen();
const char* trackOpenName();  // the open track's file name, without its folders
int trackOpenId();            // its id; -1: none open (or the list has not got to it yet)
// A track's id for its path below /TRACKS ("Rock/Song.mp3"), its exact file name, or
// words of its name or its folders' names (the first found); -1 if there is none.
int findTrack(const char* s);
// Opens the track with that id (-1 closes the open one). What played stops. False on failure.
bool selectTrack(int id);

void trackPlay();
void trackPause();  // keeps its place
void trackStop();   // back to the start (the loop's start while one is set)
bool trackPlaying();
void trackSeek(float seconds);
// The tempo in percent of the track's own, 50 to 150, at the same pitch (100: as recorded).
// A new track starts at 100.
void trackTempo(int percent);
int trackTempoPercent();
// The tempo as beats per minute; false if the track's own tempo is not known.
bool trackTempoBpm(float bpm);

// The track's own tempo as the scan found it (or as set by hand); 0: not known.
float trackBpm();
void trackShowBpm();  // prints it
bool trackBpmSure();     // the scan's confidence is 0.5 or more
int trackScanPercent();  // 100: done; -1: no track
// Sets the tempo by hand (the beat nearest to where the track is stays in place), halves
// or doubles it, sets the beats in a bar.
void trackSetBpm(float bpm);
void trackScaleBpm(bool twice);
void trackBeatsPerBar(int n);
// Loop points go to the nearest beat (on by default).
void trackSnap(bool on);
// Scans the open track again.
void trackRescan();
// Where the track is, in bars and beats; false without a beat grid.
bool trackBarBeat(int* bar, int* beat);
// Loops `bars` bars from a bar and a beat in it (both from 1); false if that is no
// section of the track or it has no beat grid. And the loop's points told that way
// (start to the nearest beat, length to the nearest bar, 1 or more); false without a
// loop or a grid.
bool trackLoopFrom(int bar, int beat, int bars);
bool trackLoopAsBars(int* bar, int* beat, int* bars);
int trackBeatsInBar();
// Loops bars first..last (bar 1 starts with the first beat).
bool trackLoopBars(int first, int last);
float trackPosition();  // seconds
float trackLength();    // seconds
// Loops from..to (seconds); false if that is no section of the track or shorter than 0.25 s.
bool trackLoop(float from, float to);
// Marks the loop's start or end where the track is now; the loop holds once both are set.
void trackLoopMark(bool end);
// The loop off, its points kept; on again with the points it had (false: there are none);
// off and its points forgotten.
void trackLoopOff();
bool trackLoopOn();
void trackLoopClear();
// The loop's points (seconds), whether it is on or off; false if there are none.
bool trackLoopPoints(float* from, float* to);
// The loop that holds (seconds); false if there is none. And: the loop's start is marked
// and waits for its end.
bool trackLoopNow(float* from, float* to);
bool trackLoopStartMarked();
bool trackPlayerLooping();  // the loop is on
// The "Track:" line of the status.
void printTrackStatus();
// The shell's place in the list: its rows, numbered, with > at the open track.
void printTracks();
// Row n (from 1) of what printTracks() shows: a folder is gone into and shown, a track is
// opened. False if there is no such row.
bool trackShellRow(int n);
void trackShellUp();                    // to the folder above, and shows it
void trackShellFind(const char* text);  // searches all folders, and shows the hits

// ---- For the desktop app's link (AppLink.cpp) ----
// The open track's path below /TRACKS ("Rock/Song.mp3"); "" if none.
const char* trackOpenPath();
// 0 no track, 1 stopped, 2 playing, 3 paused.
int trackState();
// The first beat, seconds (it counts while trackBpm() is above 0), and whether loop points
// go to the nearest beat.
float trackFirstBeat();
bool trackSnapOn();
// The loop's marked start, seconds (while trackLoopStartMarked()).
float trackLoopStartMark();
// Blocks the queue could not fill, since start.
uint32_t trackGaps();
// The open file: 0 none, 1 WAV, 2 MP3; its sample rate, channels and kbit/s (0 for a WAV).
int trackFileKind();
uint32_t trackFileRate();
int trackFileChannels();
int trackFileKbps();
// Goes up each time the list is started anew (scanTracks()).
uint32_t trackListNumber();
// Closes the open track if it is this file or lies in this folder (a path below /TRACKS,
// whatever the case): call before the file or folder is removed, replaced or moved.
void trackLetGo(const char* below);
// Files below /TRACKS were changed: the list is read anew (the open track stays open).
void trackFilesChanged();
