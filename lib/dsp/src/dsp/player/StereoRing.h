// StereoRing.h - the backing track's queue between loop() and the audio interrupt: 16-bit
// stereo frames, one writer (the background, which decodes the file ahead of time) and one
// reader (the audio interrupt, which only copies).
//
//   - The memory is handed in (frames must be a power of two; on the unit it is PSRAM, read
//     by the interrupt in one or two sequential copies per block).
//   - written() and taken() count frames since begin() and never go back, so the writer can
//     note at which count a piece of the file starts and tell from taken() what is heard.
//   - cut(keep) drops what is queued beyond the next `keep` frames, so that a seek, a new
//     loop or a tempo change is heard after `keep` frames instead of after the whole queue.
//     The frames written next are crossfaded (kFade frames) with the dropped audio they
//     replace, which is still in the memory: no click at the join.
//
// Threading model: write(), cut(), reset(), space() run in the background; read() runs in
// the audio interrupt. reset() only while the reader does not read (TrackOut::idle()).
#pragma once

#include <stdint.h>
#include <string.h>

#include <dsp/Port.h>

namespace dsp {

class StereoRing {
 public:
  static constexpr uint32_t kFade = 256;      // frames (5 ms) crossfaded after a cut
  static constexpr uint32_t kMinKeep = 1024;  // cut() never leaves less: the reader may be taking a block

  void begin(int16_t* mem, uint32_t frames) {
    mem_ = mem;
    mask_ = frames - 1;
    w_ = r_ = 0;
    fadeLen_ = fadeLeft_ = 0;
  }
  uint32_t capacity() const { return mem_ ? mask_ + 1 : 0; }

  // ---- The writer (background) ----
  uint32_t queued() const {
    const int32_t q = (int32_t)(w_ - r_);
    return q > 0 ? (uint32_t)q : 0;
  }
  uint32_t space() const { return capacity() - queued(); }
  uint32_t written() const { return w_; }

  // Queues up to n frames (interleaved L, R); returns how many fitted.
  DSP_COLD uint32_t write(const int16_t* src, uint32_t n) {
    const uint32_t room = space();
    if (n > room) n = room;
    const uint32_t w = w_;
    for (uint32_t done = 0; done < n;) {
      const uint32_t at = (w + done) & mask_;
      uint32_t run = n - done;
      if (run > capacity() - at) run = capacity() - at;
      int16_t* d = mem_ + 2 * at;
      const int16_t* s = src + 2 * done;
      if (fadeLeft_) {
        // After a cut: from the dropped audio (in the memory) over to the new.
        if (run > fadeLeft_) run = fadeLeft_;
        const int32_t len = (int32_t)fadeLen_;
        int32_t k = (int32_t)(fadeLen_ - fadeLeft_);
        for (uint32_t i = 0; i < run; i++, k++) {
          d[2 * i] = (int16_t)((d[2 * i] * (len - k) + s[2 * i] * (k + 1)) / (len + 1));
          d[2 * i + 1] = (int16_t)((d[2 * i + 1] * (len - k) + s[2 * i + 1] * (k + 1)) / (len + 1));
        }
        fadeLeft_ -= run;
      } else {
        memcpy(d, s, run * 2 * sizeof(int16_t));
      }
      done += run;
    }
    w_ = w + n;  // the reader sees the frames only now
    return n;
  }

  // Drops what is queued beyond the next keep frames; returns how many frames went.
  DSP_COLD uint32_t cut(uint32_t keep) {
    if (keep < kMinKeep) keep = kMinKeep;
    const uint32_t w = w_, r = r_;
    if ((int32_t)(w - r) <= (int32_t)keep) return 0;
    const uint32_t nw = r + keep;
    const uint32_t dropped = w - nw;
    fadeLen_ = fadeLeft_ = dropped < kFade ? dropped : kFade;
    w_ = nw;
    return dropped;
  }

  // Empties the queue. Only while the reader does not read.
  DSP_COLD void reset() {
    w_ = r_;
    fadeLen_ = fadeLeft_ = 0;
  }

  // ---- The reader (audio interrupt) ----
  uint32_t taken() const { return r_; }

  // Takes up to n frames; returns how many there were.
  uint32_t read(int16_t* dst, uint32_t n) {
    const uint32_t r = r_;
    const int32_t q = (int32_t)(w_ - r);
    if (q <= 0) return 0;
    if (n > (uint32_t)q) n = (uint32_t)q;
    const uint32_t at = r & mask_;
    const uint32_t first = n < capacity() - at ? n : capacity() - at;
    memcpy(dst, mem_ + 2 * at, first * 2 * sizeof(int16_t));
    if (first < n) memcpy(dst + 2 * first, mem_, (n - first) * 2 * sizeof(int16_t));
    r_ = r + n;
    return n;
  }

 private:
  int16_t* mem_ = nullptr;
  uint32_t mask_ = 0;
  volatile uint32_t w_ = 0, r_ = 0;
  uint32_t fadeLen_ = 0, fadeLeft_ = 0;  // the writer's
};

}  // namespace dsp
