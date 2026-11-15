// TrackSource.h - a backing track's audio as the player reads it: 16-bit stereo frames in
// file order, with a seek. WavStream.h is one; an MP3 reader is another.
#pragma once

#include <stdint.h>

namespace dsp {

class TrackSource {
 public:
  virtual ~TrackSource() {}
  virtual uint32_t sampleRate() const = 0;
  virtual uint32_t frames() const = 0;  // the whole length
  // Decodes up to maxFrames frames (interleaved L, R) from where the last read or seek
  // ended; returns how many. Fewer than asked for only at the end.
  virtual int read(int16_t* dst, int maxFrames) = 0;
  virtual bool seek(uint32_t frame) = 0;
};

}  // namespace dsp
