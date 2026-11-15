// TrackInfo.cpp - see TrackInfo.h. All of it runs in the background, from flash (DSP_COLD).
#include <dsp/player/TrackInfo.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD long TrackInfo::beatAt(double t) const { return (long)floor((t - (double)firstBeat) / period() + 1e-9); }

DSP_COLD long TrackInfo::nearestBeat(double t) const { return (long)floor((t - (double)firstBeat) / period() + 0.5); }

DSP_COLD void TrackInfo::barAndBeat(long k, int* bar, int* beat) const {
  long b = k / beatsPerBar, r = k % beatsPerBar;
  if (r < 0) {  // before the first beat: round down
    r += beatsPerBar;
    b--;
  }
  *bar = (int)b + 1;
  *beat = (int)r + 1;
}

DSP_COLD bool parseTrackInfo(const char* text, TrackInfo& info) {
  info = TrackInfo();
  bool versionOk = false;
  const char* p = text;
  while (*p) {
    const char* end = strchr(p, '\n');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    char line[96];
    if (len < sizeof(line)) {
      memcpy(line, p, len);
      line[len] = 0;
      char* value = strchr(line, ' ');
      if (value) {
        *value++ = 0;
        if (!strcmp(line, "track")) versionOk = atoi(value) == 1;
        else if (!strcmp(line, "size")) info.fileBytes = (uint32_t)strtoul(value, nullptr, 10);
        else if (!strcmp(line, "scanned")) info.scanned = atoi(value) != 0;
        else if (!strcmp(line, "bpm")) info.bpm = (float)atof(value);
        else if (!strcmp(line, "beat")) info.firstBeat = (float)atof(value);
        else if (!strcmp(line, "confidence")) info.confidence = (float)atof(value);
        else if (!strcmp(line, "beats")) info.beatsPerBar = atoi(value);
        else if (!strcmp(line, "tempo")) info.tempo = (float)atof(value);
        else if (!strcmp(line, "loop")) {
          char* second;
          info.loopFrom = (float)strtod(value, &second);
          info.loopTo = (float)strtod(second, nullptr);
        } else if (!strcmp(line, "looping")) info.looping = atoi(value) != 0;
        else if (!strcmp(line, "snap")) info.snap = atoi(value) != 0;
      }
    }
    if (!end) break;
    p = end + 1;
  }
  // Values a damaged file could bring: back to what is safe.
  if (info.beatsPerBar < 1 || info.beatsPerBar > 16) info.beatsPerBar = 4;
  if (!(info.bpm >= 20.0f && info.bpm <= 400.0f)) info.bpm = 0.0f;
  if (!(info.tempo >= 0.25f && info.tempo <= 4.0f)) info.tempo = 1.0f;
  if (!(info.loopTo > info.loopFrom) || info.loopFrom < 0.0f) info.loopFrom = info.loopTo = 0.0f;
  return versionOk;
}

DSP_COLD int formatTrackInfo(const TrackInfo& info, char* buf, int n) {
  int len = snprintf(buf, (size_t)n, "track 1\nsize %lu\nscanned %d\nbpm %.4f\nbeat %.4f\nconfidence %.2f\nbeats %d\n",
                     (unsigned long)info.fileBytes, info.scanned ? 1 : 0, (double)info.bpm, (double)info.firstBeat,
                     (double)info.confidence, info.beatsPerBar);
  if (len < n) len += snprintf(buf + len, (size_t)(n - len), "tempo %.4f\n", (double)info.tempo);
  if (len < n && info.loopTo > info.loopFrom)
    len += snprintf(buf + len, (size_t)(n - len), "loop %.3f %.3f\nlooping %d\n", (double)info.loopFrom, (double)info.loopTo,
                    info.looping ? 1 : 0);
  if (len < n) len += snprintf(buf + len, (size_t)(n - len), "snap %d\n", info.snap ? 1 : 0);
  return len < n ? len : n - 1;
}

}  // namespace dsp
