// Mp3Stream.h - an MP3 file as a TrackSource: decoded a frame at a time to 16-bit stereo,
// with a seek that lands on the sample.
//
//   - The decoder is minimp3 (vendor/minimp3.h, public domain), built for layer 3 only.
//   - open() walks the whole file once, reading only the frame headers: that gives the
//     length and a table of where every 16th frame starts, which a seek needs (frames differ
//     in size in a VBR file). No audio is decoded for it; the time it takes is the time to
//     read the file.
//   - A seek starts 10 frames before the target and throws their sound away: an MP3 frame
//     can keep part of its data in the frames before it, and the decoder's filters need a
//     run-in. After that the samples are those of a decode from the start.
//   - The timeline is the file's frames, each worth 1152 samples (576 in the half-rate
//     formats), from the first one: an encoder's start-up delay (about 25 ms of near
//     silence) is part of it, and a frame that cannot be decoded is silence of its length.
//   - A mono file goes to both sides.
//   - ID3v2 tags (titles, cover pictures) at the start are stepped over.
//   - The sample rate is the file's: the caller resamples if it is not the output's.
//
// Memory is handed in (see Memory). Decoding one frame also takes about 17 KB of stack.
// Background only (reads the file).
#pragma once

#include <stdint.h>

#include <dsp/ByteSource.h>
#include <dsp/player/TrackSource.h>

namespace dsp {

class Mp3Stream : public TrackSource {
 public:
  static constexpr int kDecoderBytes = 6720;  // the decoder's state
  static constexpr int kPcmSamples = 2304;    // one frame, two channels
  static constexpr int kMinInBytes = 8192;
  static constexpr int kIndexEvery = 16;      // frames between the seek table's entries
  static constexpr int kRunIn = 10;           // frames decoded and thrown away before a seek's target

  struct Memory {
    void* decoder;    // kDecoderBytes, aligned for floats
    uint8_t* in;      // the file's bytes pass through here
    int inBytes;      // kMinInBytes or more (16 KB is good)
    int16_t* pcm;     // kPcmSamples
    uint32_t* index;  // the seek table: one entry per kIndexEvery frames
    int indexCap;     // 8192 entries cover 57 minutes at 44.1 kHz
  };

  explicit Mp3Stream(const Memory& m) : m_(m) {}

  // Finds the frames; src must stay valid until close(). False and *err on failure.
  bool open(ByteSource& src, const char** err);
  void close() { src_ = nullptr; }
  bool isOpen() const { return src_ != nullptr; }
  int channels() const { return channels_; }
  int bitrateKbps() const { return kbps_; }  // the first frame's
  uint32_t position() const { return pos_; }  // the next frame read() gives

  uint32_t sampleRate() const override { return rate_; }
  uint32_t frames() const override { return total_; }
  int read(int16_t* dst, int maxFrames) override;
  bool seek(uint32_t frame) override;

 private:
  bool seekBytes(uint32_t offset);
  void refill();
  int nextFrame(bool decode);
  uint32_t frameOffset() const { return fileAt_ + (uint32_t)inPos_ + (uint32_t)lastOffset_; }

  Memory m_;
  ByteSource* src_ = nullptr;
  uint32_t dataStart_ = 0;   // behind the ID3v2 tag
  uint32_t fileAt_ = 0;      // the file offset of in[0]
  int inLen_ = 0, inPos_ = 0;
  bool inEof_ = false;
  int lastOffset_ = 0;       // of the frame nextFrame() found, from inPos_ before the call
  uint32_t rate_ = 0, total_ = 0, pos_ = 0;
  int channels_ = 0, kbps_ = 0, perFrame_ = 0;
  uint32_t frameCount_ = 0;
  int pcmLen_ = 0, pcmPos_ = 0;  // what is left of the frame decoded last (sample frames)
  int pcmChannels_ = 2;
};

}  // namespace dsp
