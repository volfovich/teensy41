// TrackLibrary.cpp - see TrackLibrary.h.
#include "TrackLibrary.h"

#include <Arduino.h>
#include <SD.h>

#include <dsp/player/BeatScan.h>
#include <dsp/player/Mp3Stream.h>
#include <dsp/player/ResampledSource.h>
#include <dsp/player/StereoRing.h>
#include <dsp/player/TimeStretch.h>
#include <dsp/player/TrackIndex.h>
#include <dsp/player/TrackInfo.h>
#include <dsp/player/TrackPlayer.h>
#include <dsp/player/WavStream.h>

#include "AudioChain.h"
#include "IRLibrary.h"
#include "SdSource.h"
#include "config.h"

// The list (dsp/player/TrackIndex.h): every folder and track below /TRACKS, 96 KB of
// entries and 448 KB of names.
static const char kRoot[] = "/TRACKS";
static const int kMaxEntries = 8192;
static const uint32_t kPoolBytes = 448 * 1024;
// A track's path on the card: the root, a slash, the path below it; ".trk" behind it.
static const int kTrackPath = (int)sizeof(kRoot) + dsp::TrackIndex::kPathMax;
static const int kFullPath = kTrackPath + 8;
// The queue: 2.7 s of 16-bit stereo, 512 KB. loop() can be away for about a second (an amp
// model being read, the card's free space being counted) and the track must not stop.
static const uint32_t kQueueFrames = 131072;
static const int kChunkFrames = 1024;

// In the PSRAM: the queue is the one thing there that the audio interrupt reads, one block
// at a time, front to back (128 bytes a block); the rest only loop() touches.
EXTMEM static int16_t g_queue[2 * kQueueFrames];
EXTMEM static int16_t g_chunk[2 * kChunkFrames];
EXTMEM static uint8_t g_wavScratch[4096];
EXTMEM static dsp::TrackIndex::Entry g_entries[kMaxEntries];
EXTMEM static char g_pool[kPoolBytes];
EXTMEM static uint16_t g_hits[2][dsp::TrackBrowser::kMaxHits];
static dsp::TrackIndex g_index({g_entries, kMaxEntries, g_pool, kPoolBytes});
// Where the shell and the touch screen are in the list.
static dsp::TrackBrowser g_browsers[2] = {dsp::TrackBrowser(g_index, g_hits[0]), dsp::TrackBrowser(g_index, g_hits[1])};
// The card's folders are read into the list a little at a time, from serviceTrack().
static bool g_walking = false, g_dirOpen = false;
static File g_dir;
static uint32_t g_walkStart = 0, g_walkUs = 0, g_walkSlices = 0;
// The open track: its path on the card, and its number in the list (-1: not looked up
// yet, or the list does not have it).
static bool g_open = false;
static char g_path[kTrackPath] = "";
static int g_openId = -1;

// The MP3 decoder's state and its one decoded frame, and the resampler's table and window:
// worked on all the time while a track plays, so in RAM2. The bytes on their way from the
// card and the seek table are in the PSRAM.
DMAMEM static float g_mp3Decoder[dsp::Mp3Stream::kDecoderBytes / sizeof(float)];
DMAMEM static int16_t g_mp3Pcm[dsp::Mp3Stream::kPcmSamples];
EXTMEM static uint8_t g_mp3In[16384];
static const int kMp3IndexCap = 8192;  // 57 minutes at 44.1 kHz
EXTMEM static uint32_t g_mp3Index[kMp3IndexCap];
DMAMEM static float g_resampleTable[dsp::ResampledSource::kTableFloats];
DMAMEM static float g_resampleWindow[2 * dsp::ResampledSource::kWindowFrames];
DMAMEM static int16_t g_resampleRaw[2 * dsp::ResampledSource::kReadFrames];

static dsp::StereoRing g_ring;
static File g_file;
static SdSource g_src(g_file);
static dsp::WavStream g_wav(g_wavScratch, sizeof(g_wavScratch));
static dsp::Mp3Stream g_mp3({g_mp3Decoder, g_mp3In, (int)sizeof(g_mp3In), g_mp3Pcm, g_mp3Index, kMp3IndexCap});
static dsp::ResampledSource g_resampled({g_resampleTable, g_resampleWindow, g_resampleRaw}, (uint32_t)kSampleRate);
// The tempo change (a phase vocoder): its work buffers are gone through several times for
// every 11 ms of output, so they are in RAM2. (In the PSRAM such work took three times as
// long: the audio interrupt empties the data cache every block.) The input it holds is
// read through once per window, front to back: that is in the PSRAM.
EXTMEM static int16_t g_stretchIn[2 * dsp::TimeStretch::kInFrames];
DMAMEM static float g_stretchWork[dsp::TimeStretch::kWorkFloats];
DMAMEM static uint16_t g_stretchIndex[dsp::TimeStretch::kIndexWords];
DMAMEM static int16_t g_stretchOut[2 * dsp::TimeStretch::kHop];
static dsp::TimeStretch g_stretch({g_stretchIn, g_stretchWork, g_stretchIndex, g_stretchOut});
static dsp::TrackPlayer g_player(g_ring, chain.track(), g_chunk, kChunkFrames, &g_stretch);
// The scan for tempo and beats (dsp/player/BeatScan.h) and what is known about the open
// track (dsp/player/TrackInfo.h, kept in a file beside it).
EXTMEM static dsp::BeatScan::Peak g_peaks[dsp::BeatScan::kMaxPeaks];
static dsp::BeatScan g_scan({g_peaks});
static dsp::TrackInfo g_info;
static uint32_t g_fileBytes = 0;
static bool g_scanning = false;      // the open track's scan is not done yet
static bool g_scanHasReader = false;  // the reader stands where the scan left it
static uint32_t g_scanFrame = 0;     // how far the scan is (the file's frames)
static bool g_readerMoved = false;   // the scan has moved the reader since the player last placed it
static bool g_infoDirty = false;     // the track's file is to be written
static uint32_t g_infoDirtyAt = 0;
static bool g_isMp3 = false;     // which reader has the open file
static bool g_resampling = false;  // the file's rate is not the output's
static uint32_t g_busyPercent = 0;  // of the time, serviceTrack() was at work (the last second)

// The loop's marks (frames; -1 = not set), and what scanTracks() takes up again.
static int32_t g_markA = -1, g_markB = -1;
static uint32_t g_keepFrame = 0;
static bool g_keep = false;
// A tempo set while the track plays waits here until it has stood still (setTempoRate()).
static bool g_tempoPending = false;
static uint32_t g_tempoAt = 0;

static float toSeconds(uint32_t frames) { return (float)frames / kSampleRate; }
static uint32_t toFrames(float s) { return s <= 0.0f ? 0 : (uint32_t)lroundf(s * kSampleRate); }

// "3:07.4"
static const char* timeText(uint32_t frames, char* buf, int n) {
  const uint32_t tenths = (uint32_t)((uint64_t)frames * 10 / (uint32_t)kSampleRate);
  snprintf(buf, n, "%lu:%02lu.%lu", (unsigned long)(tenths / 600), (unsigned long)(tenths / 10 % 60),
           (unsigned long)(tenths % 10));
  return buf;
}

void trackBegin() {
  g_ring.begin(g_queue, kQueueFrames);
  chain.track().begin(&g_ring);
}

static bool hasExt(const char* n, const char* ext) {
  const size_t len = strlen(n);
  return len > 4 && !strcasecmp(n + len - 4, ext);
}

static bool isTrackName(const char* n) {
  if (n[0] == '.') return false;  // hidden files, macOS "._" resource forks
  return hasExt(n, ".wav") || hasExt(n, ".mp3");
}

static void closeFile() {
  g_resampled.setSource(nullptr);
  g_wav.close();
  g_mp3.close();
  g_file.close();
}

// The open file's reader, and the source the player reads: the reader itself, or the
// resampler in front of it when the file is not at 48 kHz.
static dsp::TrackSource& fileReader() { return g_isMp3 ? (dsp::TrackSource&)g_mp3 : (dsp::TrackSource&)g_wav; }
static dsp::TrackSource& playSource() { return g_resampling ? (dsp::TrackSource&)g_resampled : fileReader(); }
static uint32_t readerPosition() { return g_isMp3 ? g_mp3.position() : g_wav.position(); }

// The open track's file name, without its folders.
static const char* openName() {
  const char* slash = strrchr(g_path, '/');
  return slash ? slash + 1 : g_path;
}

// Opens the file at path for the player's reader; false with a message if it cannot be played.
static bool openFile(const char* path) {
  const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  g_file = SD.open(path, FILE_READ);
  if (!g_file) {
    Serial.printf("Cannot open %s\n", path);
    return false;
  }
  g_fileBytes = (uint32_t)g_file.size();
  const char* err = "";
  g_isMp3 = hasExt(path, ".mp3");
  // An MP3 is walked through once here, for its length and seek table.
  const bool ok = g_isMp3 ? g_mp3.open(g_src, &err) : g_wav.open(g_src, &err);
  if (!ok) {
    Serial.printf("%s: %s\n", name, err);
  } else {
    g_resampling = fileReader().sampleRate() != (uint32_t)kSampleRate;
    if (!g_resampling || g_resampled.setSource(&fileReader())) return true;
    Serial.printf("%s: %lu Hz cannot be played (8000 to 96000 Hz can).\n", name,
                  (unsigned long)fileReader().sampleRate());
  }
  closeFile();
  return false;
}

static void printLoop() {
  char a[16], b[16];
  Serial.printf("Track loop %s - %s\n", timeText(g_player.loopA(), a, sizeof(a)), timeText(g_player.loopB(), b, sizeof(b)));
}

// ---- The file beside the track ("song.mp3.trk") ----

static void infoPath(char* path, int n) { snprintf(path, n, "%s.trk", g_path); }

// What is kept about the open track; nothing kept, or kept for another file of that name:
// a blank sheet.
static void loadInfo() {
  char path[kFullPath], text[640];
  infoPath(path, sizeof(path));
  g_info = dsp::TrackInfo();
  File f = SD.open(path, FILE_READ);
  if (f) {
    const int n = f.read(text, sizeof(text) - 1);
    f.close();
    text[n > 0 ? n : 0] = 0;
    dsp::TrackInfo kept;
    if (dsp::parseTrackInfo(text, kept) && kept.fileBytes == g_fileBytes) g_info = kept;
  }
  g_info.fileBytes = g_fileBytes;
  g_infoDirty = false;
}

static void saveInfo() {
  g_infoDirty = false;
  if (!g_open) return;
  char path[kFullPath], text[640];
  infoPath(path, sizeof(path));
  const int len = dsp::formatTrackInfo(g_info, text, sizeof(text));
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f || f.write(text, len) != (size_t)len) Serial.printf("Cannot write %s\n", path);
  f.close();
}

// Written a moment after the last change, not on every step of a dial.
static void infoChanged() {
  g_infoDirty = true;
  g_infoDirtyAt = millis();
}

// ---- The scan for tempo and beats ----

static void startScan() {
  g_info.scanned = false;
  g_scanning = true;
  g_scanHasReader = false;
  g_scanFrame = 0;
  g_scan.begin(fileReader().sampleRate());
}

static void printBpm() {
  if (!g_info.scanned) Serial.printf("Track BPM: not known yet (scanned %lu%%; the scan runs while the track is not playing)\n",
                                     (unsigned long)(100.0f * g_scanFrame / (float)(fileReader().frames() + 1)));
  else if (!g_info.hasGrid()) Serial.println("Track BPM: no steady beat found (track bpm <value> sets one)");
  else
    Serial.printf("Track BPM %.2f%s, %d beats in a bar, first beat at %.3f s\n", g_info.bpm,
                  g_info.confidence >= 0.5f ? "" : " (unsure)", g_info.beatsPerBar, g_info.firstBeat);
}

static void scanDone() {
  const uint32_t t0 = millis();
  const dsp::BeatScan::Result r = g_scan.finish();
  g_scanning = false;
  g_scanHasReader = false;
  g_info.scanned = true;
  g_info.bpm = r.found ? r.bpm : 0.0f;
  g_info.firstBeat = r.found ? r.firstBeat : 0.0f;
  g_info.confidence = r.confidence;
  saveInfo();
  Serial.printf("Scan done (%lu ms to work it out). ", (unsigned long)(millis() - t0));
  printBpm();
}

// A few ms of the scan: the file's own audio (not resampled) through the beat finder.
static void scanSlice() {
  dsp::TrackSource& reader = fileReader();
  if (!g_scanHasReader) {
    reader.seek(g_scanFrame);
    g_scanHasReader = true;
    g_readerMoved = true;
  }
  const uint32_t t0 = micros();
  do {
    const int got = reader.read(g_chunk, kChunkFrames);
    if (got > 0) {
      g_scan.feed(g_chunk, got);
      g_scanFrame += (uint32_t)got;
    }
    if (got < kChunkFrames) {
      scanDone();
      return;
    }
  } while (micros() - t0 < 4000);
}

// Before the player reads again: the scan may have left the reader anywhere.
static void placeReader() {
  if (!g_readerMoved) return;
  g_readerMoved = false;
  g_scanHasReader = false;
  g_player.seek(g_player.position());
}

// ---- The beat grid ----

// A loop point: the nearest beat, if the track has a grid and snapping is on; otherwise
// as it is.
static uint32_t snapped(uint32_t frame) {
  if (!g_info.hasGrid() || !g_info.snap) return frame;
  const double t = (double)frame / kSampleRate;
  const double at = g_info.beatTime(g_info.nearestBeat(t));
  return at <= 0.0 ? 0 : (uint32_t)lround(at * kSampleRate);
}

void trackCardGoing() {
  g_keep = g_open;
  if (g_dirOpen) g_dir.close();
  g_dirOpen = false;
  if (!g_keep) return;
  g_keepFrame = readerPosition();
  // The readers and the resampler keep what they hold; only the file is let go of.
  g_file.close();
}

static uint32_t g_listNumber = 0;

void scanTracks() {
  // The list starts anew and is read from serviceTrack(); the numbers it gave are void.
  g_listNumber++;
  if (g_dirOpen) g_dir.close();
  g_dirOpen = false;
  g_index.begin();
  g_browsers[0].reset();
  g_browsers[1].reset();
  g_openId = -1;
  g_walking = irSdOk();
  g_walkStart = millis();
  g_walkUs = g_walkSlices = 0;
  if (!g_walking) g_index.endFolder();  // no card: an empty list
  if (!g_keep) return;
  // The track that was open: the same file again under the same reader, which reads on
  // from where it was. (The resampler in front keeps the input it holds, and the queue
  // keeps playing meanwhile, so nothing is heard of this.)
  g_keep = false;
  g_file = SD.open(g_path, FILE_READ);
  const char* err = "";
  if (g_file && (g_isMp3 ? g_mp3.open(g_src, &err) : g_wav.open(g_src, &err)) && fileReader().seek(g_keepFrame)) {
    g_scanHasReader = false;  // a scan under way finds its place again
    return;
  }
  g_player.setSource(nullptr);
  g_scanning = false;
  closeFile();
  g_open = false;
  g_markA = g_markB = -1;
  Serial.println("The backing track's file has gone: no track.");
}

// A few ms of reading the card's folders into the list.
static void walkSlice() {
  const uint32_t t0 = micros();
  while (!g_index.done() && micros() - t0 < 3000) {
    if (!g_dirOpen) {
      char path[kFullPath], below[dsp::TrackIndex::kPathMax];
      g_index.pathOf(g_index.reading(), below, sizeof(below));
      if (below[0]) snprintf(path, sizeof(path), "%s/%s", kRoot, below);
      else strlcpy(path, kRoot, sizeof(path));
      g_dir = SD.open(path);
      if (!g_dir || !g_dir.isDirectory()) {  // gone, or no /TRACKS at all
        g_dir.close();
        g_index.endFolder();
        continue;
      }
      g_dirOpen = true;
    }
    File e = g_dir.openNextFile();
    if (!e) {
      g_dir.close();
      g_dirOpen = false;
      g_index.endFolder();
      continue;
    }
    const char* name = e.name();
    const bool folder = e.isDirectory();
    // Hidden folders and files are left out, and files that are not tracks.
    if (folder ? name[0] != '.' : isTrackName(name)) g_index.add(name, folder);
    e.close();
  }
  g_walkUs += micros() - t0;
  g_walkSlices++;
  if (!g_index.done()) return;
  g_walking = false;
  if (g_index.files() || g_index.skipped())
    Serial.printf("Found %d backing track(s) in %d folder(s) below %s (%lu ms; reading took %lu ms of it, in %lu parts).\n", g_index.files(),
                  g_index.entries() - 1 - g_index.files(), kRoot, (unsigned long)(millis() - g_walkStart),
                  (unsigned long)(g_walkUs / 1000), (unsigned long)g_walkSlices);
  if (g_index.skipped()) Serial.printf("%d left out: a name of %d characters or more, a path of %d or more, or more than %d folders deep.\n",
                                       g_index.skipped(), dsp::TrackIndex::kNameMax, dsp::TrackIndex::kPathMax, dsp::TrackIndex::kMaxDepth);
  if (g_index.full()) Serial.printf("The list is full (%d folders and tracks, or their names' %lu KB): the rest is left out.\n",
                                    kMaxEntries, (unsigned long)(kPoolBytes / 1024));
}

void serviceTrack() {
  static uint32_t busyUs = 0, since = 0;
  const bool was = g_player.state() == dsp::TrackPlayer::kPlaying;
  if (g_walking) walkSlice();
  const uint32_t t0 = micros();
  // The scan and the player share the file's reader: the scan runs while nothing plays.
  if (g_scanning && !was && g_open) {
    scanSlice();
    if (!g_scanning) placeReader();
  } else {
    g_player.service();
  }
  busyUs += micros() - t0;
  if (g_tempoPending && millis() - g_tempoAt >= 250) {
    g_tempoPending = false;
    g_player.setTempo(g_info.tempo);
  }
  if (g_infoDirty && millis() - g_infoDirtyAt > 2000) saveInfo();
  if (millis() - since >= 1000) {
    g_busyPercent = busyUs / (10 * (millis() - since));
    since = millis();
    busyUs = 0;
  }
  if (was && g_player.state() == dsp::TrackPlayer::kStopped) Serial.println("TRACK END");
}

const dsp::TrackIndex& trackIndex() { return g_index; }
dsp::TrackBrowser& trackBrowser(bool screen) { return g_browsers[screen ? 1 : 0]; }
bool trackReadingCard() { return g_walking; }
bool trackOpen() { return g_open; }
const char* trackOpenName() { return g_open ? openName() : ""; }

int trackOpenId() {
  // Looked up once the list has been read (the track may have been opened before that).
  if (g_open && g_openId == -1 && g_index.done()) {
    g_openId = g_index.findPath(g_path + sizeof(kRoot));
    if (g_openId < 0) g_openId = -2;  // not in the list: not looked up again
  }
  return g_open && g_openId >= 0 ? g_openId : -1;
}

int findTrack(const char* s) {
  int id = g_index.findPath(s);
  if (id > 0 && !g_index.isFolder(id)) return id;
  id = g_index.findName(s);
  if (id >= 0) return id;
  uint16_t hit;
  return g_index.search(s, &hit, 1, nullptr) ? hit : -1;
}

bool selectTrack(int id) {
  if (g_infoDirty) saveInfo();
  // The player lets go of the old file before it is closed.
  g_player.setSource(nullptr);
  g_markA = g_markB = -1;
  g_scanning = false;
  g_readerMoved = false;
  if (g_open) {
    closeFile();
    g_open = false;
  }
  g_openId = -1;
  if (id < 0) {
    Serial.println("No track.");
    return true;
  }
  const uint32_t t0 = millis();
  char below[dsp::TrackIndex::kPathMax];
  if (g_index.isFolder(id) || !g_index.pathOf(id, below, sizeof(below)) || !below[0]) return false;
  snprintf(g_path, sizeof(g_path), "%s/%s", kRoot, below);
  if (!openFile(g_path)) return false;
  g_open = true;
  g_openId = id;
  g_player.setSource(&playSource());
  // As the track was left: its tempo and its loop. A track not scanned yet is scanned now,
  // while it is not playing.
  loadInfo();
  g_player.setTempo(g_info.tempo);
  g_info.tempo = g_player.tempo();
  g_tempoPending = false;
  // The loop's points as they were left; the loop itself only if it was on.
  if (g_info.loopTo > 0.0f) {
    if (g_info.looping && g_player.setLoop(toFrames(g_info.loopFrom), toFrames(g_info.loopTo))) {
      g_markA = (int32_t)g_player.loopA();
      g_markB = (int32_t)g_player.loopB();
    } else if (!g_info.looping && toFrames(g_info.loopTo) <= g_player.length()) {
      g_markA = (int32_t)toFrames(g_info.loopFrom);
      g_markB = (int32_t)toFrames(g_info.loopTo);
    } else {
      g_info.loopFrom = g_info.loopTo = 0.0f;  // no section of this file
    }
  }
  if (!g_info.scanned) startScan();
  char len[16];
  Serial.printf("Track: %s, %s, ", below, timeText(g_player.length(), len, sizeof(len)));
  if (g_isMp3) {
    Serial.printf("MP3 %d kbps, %d ch", g_mp3.bitrateKbps(), g_mp3.channels());
  } else {
    const dsp::WavInfo& w = g_wav.info();
    Serial.printf("%u ch, %u-bit %s", w.channels, w.bitsPerSample, w.format == 3 ? "float" : "PCM");
  }
  if (g_resampling) Serial.printf(", %lu Hz (resampled)", (unsigned long)fileReader().sampleRate());
  Serial.printf(", opened in %lu ms\n", (unsigned long)(millis() - t0));
  if (g_info.scanned) printBpm();
  if (trackTempoPercent() != 100) Serial.printf("Track tempo %d%%\n", trackTempoPercent());
  if (g_player.looping()) printLoop();
  return true;
}

void trackPlay() {
  if (!g_open) {
    Serial.println("No track is open (track <n|name>; 'track' lists them).");
    return;
  }
  chain.track().clearUnderruns();
  placeReader();
  g_player.play();
  char at[16];
  Serial.printf("TRACK PLAY: %s, from %s\n", openName(), timeText(g_player.position(), at, sizeof(at)));
}

void trackPause() {
  if (g_player.state() != dsp::TrackPlayer::kPlaying) return;
  g_player.pause();
  char at[16];
  Serial.printf("TRACK PAUSE at %s\n", timeText(g_player.position(), at, sizeof(at)));
}

void trackStop() {
  if (!g_open) return;
  g_player.stop();
  Serial.println("TRACK STOP");
}

bool trackPlaying() { return g_player.state() == dsp::TrackPlayer::kPlaying; }

void trackSeek(float seconds) {
  if (!g_open) return;
  uint32_t to = toFrames(seconds);
  if (to > g_player.length()) to = g_player.length();
  g_player.seek(to);
  char at[16];
  Serial.printf("Track to %s\n", timeText(to, at, sizeof(at)));  // heard 50 ms from now
}

// A change of tempo while the track plays makes the player start anew from where it is
// (a seek, the queue cut, the tempo change's memory emptied). A held button gives ten
// changes a second: the player gets the tempo once it has stood still for a moment.

static void setTempoRate(float rate) {
  if (rate < dsp::TimeStretch::kMinRate) rate = dsp::TimeStretch::kMinRate;
  if (rate > dsp::TimeStretch::kMaxRate) rate = dsp::TimeStretch::kMaxRate;
  if (rate > 0.995f && rate < 1.005f) rate = 1.0f;
  g_info.tempo = rate;
  if (trackPlaying()) {
    g_tempoPending = true;
    g_tempoAt = millis();
  } else {
    g_tempoPending = false;
    g_player.setTempo(rate);
  }
  infoChanged();
  Serial.printf("Track tempo %d%%", trackTempoPercent());
  if (g_info.hasGrid()) Serial.printf(" (%.1f BPM)", g_info.bpm * g_info.tempo);
  Serial.println();
}

void trackTempo(int percent) {
  if (g_open) setTempoRate(percent / 100.0f);
}

bool trackTempoBpm(float bpm) {
  if (!g_open || !g_info.hasGrid()) return false;
  setTempoRate(bpm / g_info.bpm);
  return true;
}

float trackBpm() { return g_open && g_info.scanned ? g_info.bpm : 0.0f; }
void trackShowBpm() {
  if (!g_open) Serial.println("No track is open.");
  else printBpm();
}
bool trackBpmSure() { return g_info.confidence >= 0.5f; }
int trackScanPercent() {
  if (!g_open) return -1;
  if (!g_scanning) return 100;
  return (int)(100.0f * g_scanFrame / (float)(fileReader().frames() + 1));
}

void trackSetBpm(float bpm) {
  if (!g_open || bpm < 30.0f || bpm > 300.0f) return;
  // The beat nearest to where the track is stays where it is; the others move.
  const double here = trackPosition();
  const double keep = g_info.hasGrid() ? g_info.beatTime(g_info.nearestBeat(here)) : here;
  g_info.bpm = bpm;
  const double period = g_info.period();
  g_info.firstBeat = (float)(keep - floor(keep / period) * period);
  g_info.scanned = true;  // a tempo given by hand stands; the scan does not run over it
  g_scanning = false;
  infoChanged();
  printBpm();
}

void trackScaleBpm(bool twice) {
  if (!g_open || !g_info.hasGrid()) return;
  g_info.bpm = twice ? g_info.bpm * 2.0f : g_info.bpm * 0.5f;  // the first beat stays a beat
  infoChanged();
  printBpm();
}

void trackBeatsPerBar(int n) {
  if (!g_open || n < 1 || n > 16) return;
  g_info.beatsPerBar = n;
  infoChanged();
  printBpm();
}

void trackSnap(bool on) {
  if (!g_open) return;
  g_info.snap = on;
  infoChanged();
  Serial.printf("Loop points %s\n", on ? "go to the nearest beat" : "stay where they are set");
}

void trackRescan() {
  if (!g_open) return;
  g_info.bpm = 0.0f;
  startScan();
  Serial.println("Scanning for tempo and beats (while the track is not playing).");
}

bool trackBarBeat(int* bar, int* beat) {
  if (!g_open || !g_info.hasGrid()) return false;
  g_info.barAndBeat(g_info.beatAt(trackPosition()), bar, beat);
  return true;
}

int trackTempoPercent() { return (int)lroundf((g_open ? g_info.tempo : g_player.tempo()) * 100.0f); }

float trackPosition() { return toSeconds(g_player.position()); }
float trackLength() { return toSeconds(g_player.length()); }


// The loop as it now is goes into the track's file: its points while it holds; switched
// off, the points it had stay.
static void loopChanged() {
  g_info.looping = g_player.looping();
  if (g_info.looping) {
    g_info.loopFrom = toSeconds(g_player.loopA());
    g_info.loopTo = toSeconds(g_player.loopB());
  }
  infoChanged();
}

bool trackLoop(float from, float to) {
  if (!g_open || !g_player.setLoop(toFrames(from), toFrames(to))) return false;
  g_markA = (int32_t)g_player.loopA();
  g_markB = (int32_t)g_player.loopB();
  loopChanged();
  printLoop();
  return true;
}

bool trackLoopFrom(int bar, int beat, int bars) {
  if (!g_open || !g_info.hasGrid() || bar < 1 || beat < 1 || beat > g_info.beatsPerBar || bars < 1) return false;
  const long first = g_info.barLine(bar) + (beat - 1);
  const double from = g_info.beatTime(first);
  double to = g_info.beatTime(first + (long)bars * g_info.beatsPerBar);
  if (from < 0.0 || from >= trackLength()) return false;
  if (to > trackLength()) to = trackLength();  // the last bar may run past the file's end
  return trackLoop((float)from, (float)to);
}

bool trackLoopAsBars(int* bar, int* beat, int* bars) {
  if (!g_open || !g_info.hasGrid() || !(g_info.loopTo > 0.0f)) return false;
  g_info.barAndBeat(g_info.nearestBeat(g_info.loopFrom), bar, beat);
  const double beats = ((double)g_info.loopTo - (double)g_info.loopFrom) / g_info.period();
  const int n = (int)lround(beats / g_info.beatsPerBar);
  *bars = n < 1 ? 1 : n;
  return true;
}

int trackBeatsInBar() { return g_info.beatsPerBar; }

bool trackLoopBars(int first, int last) {
  if (!g_open || !g_info.hasGrid() || last < first) return false;
  const double from = g_info.beatTime(g_info.barLine(first)), to = g_info.beatTime(g_info.barLine(last + 1));
  if (from < 0.0) return false;
  double end = to;
  if (end > trackLength()) end = trackLength();  // the last bar may run past the file's end
  return trackLoop((float)from, (float)end);
}

void trackLoopMark(bool end) {
  if (!g_open) return;
  const int32_t now = (int32_t)snapped(g_player.position());
  (end ? g_markB : g_markA) = now;
  char at[16];
  Serial.printf("Loop %s at %s\n", end ? "end" : "start", timeText((uint32_t)now, at, sizeof(at)));
  if (g_markA < 0 || g_markB < 0) return;
  if (g_player.setLoop((uint32_t)g_markA, (uint32_t)g_markB)) {
    loopChanged();
    printLoop();
  } else {
    Serial.println("That is no loop yet: the end must be 0.25 s or more after the start.");
  }
}

bool trackLoopNow(float* from, float* to) {
  if (!g_player.looping()) return false;
  *from = toSeconds(g_player.loopA());
  *to = toSeconds(g_player.loopB());
  return true;
}

bool trackLoopPoints(float* from, float* to) {
  if (!g_open || !(g_info.loopTo > 0.0f)) return false;
  *from = g_info.loopFrom;
  *to = g_info.loopTo;
  return true;
}

bool trackLoopStartMarked() { return g_markA >= 0 && g_markB < 0; }
bool trackPlayerLooping() { return g_player.looping(); }

void trackLoopOff() {
  if (!g_open || !g_player.looping()) return;
  g_player.clearLoop();
  loopChanged();
  Serial.println("Track loop off (its points are kept: track loop on)");
}

bool trackLoopOn() {
  if (!g_open || !(g_info.loopTo > 0.0f) || !g_player.setLoop(toFrames(g_info.loopFrom), toFrames(g_info.loopTo))) return false;
  g_markA = (int32_t)g_player.loopA();
  g_markB = (int32_t)g_player.loopB();
  loopChanged();
  printLoop();
  return true;
}

void trackLoopClear() {
  if (!g_open) return;
  g_markA = g_markB = -1;
  g_player.clearLoop();
  g_info.loopFrom = g_info.loopTo = 0.0f;
  loopChanged();
  Serial.println("Track loop off, its points forgotten");
}

// ---- For the desktop app's link ----

const char* trackOpenPath() { return g_open ? g_path + sizeof(kRoot) : ""; }
int trackState() { return g_open ? (int)g_player.state() : 0; }
float trackFirstBeat() { return g_info.firstBeat; }
bool trackSnapOn() { return g_info.snap; }
float trackLoopStartMark() { return g_markA >= 0 ? toSeconds((uint32_t)g_markA) : 0.0f; }
uint32_t trackGaps() { return chain.track().underruns(); }
int trackFileKind() { return !g_open ? 0 : g_isMp3 ? 2 : 1; }
uint32_t trackFileRate() { return g_open ? fileReader().sampleRate() : 0; }
int trackFileChannels() { return !g_open ? 0 : g_isMp3 ? g_mp3.channels() : (int)g_wav.info().channels; }
int trackFileKbps() { return g_open && g_isMp3 ? g_mp3.bitrateKbps() : 0; }
uint32_t trackListNumber() { return g_listNumber; }

void trackLetGo(const char* below) {
  if (!g_open) return;
  const char* open = trackOpenPath();
  const size_t n = strlen(below);
  // The file itself, or a folder on the way to it.
  if (strncasecmp(open, below, n) || (open[n] && open[n] != '/')) return;
  g_infoDirty = false;  // nothing is written beside a file that is going
  selectTrack(-1);
}

void trackFilesChanged() {
  if (g_infoDirty) saveInfo();
  scanTracks();
}

// The shell's place in the list: the rows numbered, folders with a slash, > at the open
// track. A search's hits are shown with their folders.
void printTracks() {
  const dsp::TrackBrowser& b = g_browsers[0];
  char path[dsp::TrackIndex::kPathMax];
  if (b.searching()) {
    Serial.printf("\"%s\": %d found%s\n", b.text(), b.rows(), b.more() ? " and more (only these are shown)" : "");
  } else {
    g_index.pathOf(b.folder(), path, sizeof(path));
    Serial.printf("%s%s%s: %d\n", kRoot, path[0] ? "/" : "", path, b.rows());
  }
  const int open = trackOpenId();
  for (int i = 0; i < b.rows(); i++) {
    const int id = b.row(i);
    if (b.searching()) g_index.pathOf(id, path, sizeof(path));
    Serial.printf("%c %4d  %s%s\n", id == open ? '>' : ' ', i + 1, b.searching() ? path : g_index.name(id),
                  g_index.isFolder(id) ? "/" : "");
  }
  if (g_walking) Serial.printf("(The card is still being read: %d tracks so far.)\n", g_index.files());
  else if (!g_index.files()) Serial.printf("No .wav or .mp3 files below %s\n", kRoot);
  else if (!b.searching()) Serial.println("track <n>: open that row; track ..: up a folder; track find <text>: search all folders");
}

bool trackShellRow(int n) {
  dsp::TrackBrowser& b = g_browsers[0];
  if (b.row(n - 1) < 0) return false;
  const int id = b.enter(n - 1);
  if (id < 0) printTracks();  // a folder: what is in it
  else selectTrack(id);
  return true;
}

void trackShellUp() {
  g_browsers[0].up();
  printTracks();
}

void trackShellFind(const char* text) {
  g_browsers[0].find(text);
  printTracks();
}

void printTrackStatus() {
  if (!g_open) {
    Serial.printf("Track:     none (%d below %s%s)\n", g_index.files(), kRoot, g_walking ? ", the card is still being read" : "");
    return;
  }
  static const char* const kStates[] = {"none", "stopped", "playing", "paused"};
  char at[16], len[16], a[16], b[16];
  Serial.printf("Track:     %s %s, %s / %s", kStates[g_player.state()], g_path + sizeof(kRoot),
                timeText(g_player.position(), at, sizeof(at)), timeText(g_player.length(), len, sizeof(len)));
  if (g_player.looping())
    Serial.printf(", loop %s - %s", timeText(g_player.loopA(), a, sizeof(a)), timeText(g_player.loopB(), b, sizeof(b)));
  else if (g_info.loopTo > 0.0f)
    Serial.printf(", loop %s - %s (off)", timeText(toFrames(g_info.loopFrom), a, sizeof(a)), timeText(toFrames(g_info.loopTo), b, sizeof(b)));
  if (trackTempoPercent() != 100) Serial.printf(", tempo %d%%", trackTempoPercent());
  int bar, beat;
  if (g_scanning) Serial.printf(", scanning for the tempo %d%%", trackScanPercent());
  else if (trackBarBeat(&bar, &beat)) Serial.printf(", %.2f BPM%s, bar %d beat %d", g_info.bpm, trackBpmSure() ? "" : " (unsure)", bar, beat);
  else Serial.print(", no beat found");
  Serial.printf(", queued %.1f s, gaps %lu, reading takes %lu%% of the time\n", toSeconds(g_ring.queued()),
                (unsigned long)chain.track().underruns(), (unsigned long)g_busyPercent);
}
