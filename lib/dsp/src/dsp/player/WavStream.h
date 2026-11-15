// WavStream.h - a WAV file as a TrackSource: decoded a piece at a time to 16-bit stereo,
// so a file of any length plays without being loaded.
//
//   - The header is read by parseWav() (dsp/conv/IRLoader.h): PCM 8/16/24/32-bit, float
//     32/64, EXTENSIBLE.
//   - A mono file goes to both sides; of more than two channels the first two are taken.
//   - More than 16 bits are cut to 16 (the queue holds 16-bit frames); floats are clipped
//     to full scale.
//   - A 16-bit stereo file, the usual case, is read straight into the caller's buffer.
//     Everything else passes through the scratch buffer handed to the constructor (512
//     bytes or more; a few KB means fewer reads).
//   - The sample rate is the file's: the caller resamples if it is not the output's.
//
// Background only (reads the file).
#pragma once

#include <stdint.h>

#include <dsp/ByteSource.h>
#include <dsp/conv/IRLoader.h>
#include <dsp/player/TrackSource.h>

namespace dsp {

class WavStream : public TrackSource {
 public:
  WavStream(uint8_t* scratch, int scratchBytes) : scratch_(scratch), scratchBytes_(scratchBytes) {}

  // Reads the header; src must stay valid until close(). False and *err on failure.
  bool open(ByteSource& src, const char** err);
  void close() { src_ = nullptr; }
  bool isOpen() const { return src_ != nullptr; }
  const WavInfo& info() const { return info_; }
  uint32_t position() const { return pos_; }  // the next frame read() gives

  uint32_t sampleRate() const override { return info_.sampleRate; }
  uint32_t frames() const override { return info_.frames; }
  int read(int16_t* dst, int maxFrames) override;
  bool seek(uint32_t frame) override;

 private:
  ByteSource* src_ = nullptr;
  WavInfo info_;
  uint32_t pos_ = 0;  // the next frame
  uint8_t* scratch_;
  int scratchBytes_;
};

}  // namespace dsp
