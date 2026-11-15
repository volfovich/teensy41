// TrackPlayer.h - the backing track's background side: reads a TrackSource ahead of time
// into the queue (StereoRing.h) that the audio interrupt plays from (TrackOut.h), and does
// play, pause, stop, seek and looping a section.
//
//   source -> [loop: at B jump to A, crossfaded] -> [tempo: TimeStretch] -> queue
//          -> (interrupt) TrackOut
//
//   - service() is called from the main loop as often as it comes round. It keeps the
//     queue full, so the loop may be away for nearly the queue's length (a file being
//     loaded, a screen being drawn) without a gap.
//   - What is done to a playing track (seek, a new loop) is heard after kKeepFrames
//     (50 ms): the rest of the queue is dropped and refilled, crossfaded into what it
//     replaces.
//   - A loop plays A..B (frames, B not included) round and round. At B the first
//     kSpliceFrames after A are crossfaded with what follows B in the file, so the join
//     does not click. The shortest loop is kMinLoopFrames.
//   - position() is what is heard now: the player notes at which queue count each piece
//     of the file starts and reads the interrupt's count.
//   - At the end of the file, without a loop, the track stops and goes back to the start.
//   - The tempo (setTempo(), 0.5 to 1.5 of the track's own) is changed by TimeStretch.h, at
//     the same pitch. At exactly 1 the stretcher is not used and the file's samples go to
//     the queue as they are. A change on a playing track is heard after 50 ms, like a seek.
//
// Positions are in the source's frames. Background only; the two classes it drives carry
// what the interrupt needs.
#pragma once

#include <stdint.h>

#include <dsp/player/StereoRing.h>
#include <dsp/player/TimeStretch.h>
#include <dsp/player/TrackOut.h>
#include <dsp/player/TrackSource.h>

namespace dsp {

class TrackPlayer {
 public:
  enum State : uint8_t { kEmpty, kStopped, kPlaying, kPaused };
  static constexpr int kSpliceFrames = 256;          // the loop's crossfade (5 ms)
  static constexpr uint32_t kMinLoopFrames = 12000;  // 0.25 s
  static constexpr uint32_t kKeepFrames = 2400;      // 50 ms: how soon a change is heard
  static constexpr uint32_t kTempoKeepFrames = 4800; // 100 ms: how soon a new tempo is heard
  static constexpr uint32_t kStartFrames = 4800;     // queued before the output starts

  // chunk: the player's work buffer, chunkFrames stereo frames (>= kSpliceFrames, and
  // >= TimeStretch::kHop with a stretcher). stretch: for setTempo(); without one the tempo
  // stays the track's own.
  TrackPlayer(StereoRing& ring, TrackOut& out, int16_t* chunk, int chunkFrames, TimeStretch* stretch = nullptr)
      : ring_(ring), out_(out), chunk_(chunk), chunkFrames_(chunkFrames), stretch_(stretch) {}

  // The track to play (nullptr: none). Stops; the old source is not touched again.
  void setSource(TrackSource* src);

  void play();
  void pause();
  void stop();  // and back to the start (the loop's start while one is set)
  void seek(uint32_t frame);
  // Loops a..b; false if that is shorter than kMinLoopFrames or outside the track.
  bool setLoop(uint32_t a, uint32_t b);
  void clearLoop();
  // The tempo as a part of the track's own (1 = as recorded); held to 0.5 .. 1.5.
  void setTempo(float rate);
  float tempo() const { return rate_; }

  // Fills the queue (up to maxChunks work buffers per call) and starts or ends the output.
  void service(int maxChunks = 4);

  State state() const { return state_; }
  uint32_t length() const { return src_ ? src_->frames() : 0; }
  uint32_t position() const;
  bool looping() const { return loopB_ > loopA_; }
  uint32_t loopA() const { return loopA_; }
  uint32_t loopB() const { return loopB_; }

 private:
  struct Piece {
    uint32_t at;   // the queue's count where it starts
    uint32_t src;  // the source frame there
    float rate;    // source frames per queued frame from there on (the tempo)
  };
  static constexpr int kMaxPieces = 16;

  bool fill();
  bool stretching() const { return stretch_ && rate_ != 1.0f; }
  int readSource(int maxFrames, bool* atLoopEnd, bool* ended);
  void jumpToLoopStart(bool tail, uint32_t at);
  void resync();
  void restartAt(uint32_t frame, uint32_t keep = kKeepFrames);
  void notePiece(uint32_t at, uint32_t src);
  uint32_t frameAt(uint32_t count) const;

  StereoRing& ring_;
  TrackOut& out_;
  int16_t* chunk_;
  int chunkFrames_;
  TimeStretch* stretch_;
  float rate_ = 1.0f;
  bool srcEnded_ = false;       // the source has ended; the stretcher still holds sound
  TrackSource* src_ = nullptr;
  State state_ = kEmpty;
  uint32_t srcPos_ = 0;         // the source's next frame
  uint32_t nextPos_ = 0;        // where to start after the queue is emptied
  bool flushPending_ = false;   // empty the queue once the output is idle
  bool eof_ = false;            // everything up to the end is queued
  uint32_t loopA_ = 0, loopB_ = 0;
  int spliceLeft_ = 0;
  int16_t tail_[2 * kSpliceFrames];  // what follows B, for the loop's crossfade
  Piece pieces_[kMaxPieces];
  int pieceCount_ = 0;
};

}  // namespace dsp
