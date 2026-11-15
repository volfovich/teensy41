// Host unit tests for dsp/player/TrackPlayer.h (the backing-track player: play, pause, stop,
// seek, looping a section), with the queue and the output it drives.
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <dsp/player/TrackPlayer.h>

using dsp::StereoRing;
using dsp::TrackOut;
using dsp::TrackPlayer;

void setUp() {}
void tearDown() {}

// A track whose left sample is its own frame number (so what is heard says where it is
// from) and whose right is the negative. Up to 30000 frames.
struct NumberTrack : dsp::TrackSource {
    uint32_t n, pos = 0;
    explicit NumberTrack(uint32_t frames) : n(frames) {}
    uint32_t sampleRate() const override { return 48000; }
    uint32_t frames() const override { return n; }
    int read(int16_t* dst, int maxFrames) override {
        int got = 0;
        for (; got < maxFrames && pos < n; got++, pos++) {
            dst[2 * got] = (int16_t)pos;
            dst[2 * got + 1] = (int16_t)-(int)pos;
        }
        return got;
    }
    bool seek(uint32_t frame) override {
        pos = frame > n ? n : frame;
        return true;
    }
};

// The player with its queue and output; block() is one pass of the main loop followed by
// one audio block.
struct Rig {
    static constexpr int kB = 32;
    static constexpr int kFadeIn = 480;  // frames until the output is at full volume
    std::vector<int16_t> mem = std::vector<int16_t>(2 * 32768);
    std::vector<int16_t> chunk = std::vector<int16_t>(2 * 1024);
    std::vector<int16_t> stretchIn = std::vector<int16_t>(2 * dsp::TimeStretch::kInFrames);
    std::vector<float> stretchWork = std::vector<float>(dsp::TimeStretch::kWorkFloats);
    std::vector<uint16_t> stretchIndex = std::vector<uint16_t>(dsp::TimeStretch::kIndexWords);
    std::vector<int16_t> stretchOut = std::vector<int16_t>(2 * dsp::TimeStretch::kHop);
    StereoRing ring;
    TrackOut out{48000.0f};
    dsp::TimeStretch stretcher{dsp::TimeStretch::Memory{stretchIn.data(), stretchWork.data(), stretchIndex.data(), stretchOut.data()}};
    // With a stretcher, as on the unit: at the track's own tempo it is not used.
    TrackPlayer player{ring, out, chunk.data(), 1024, &stretcher};
    std::vector<int> heard;  // the left side of every frame that was output, as 16 bits
    int idleBlocks = 0;

    Rig() {
        ring.begin(mem.data(), 32768);
        out.begin(&ring);
    }
    void audio() {
        float l[kB], r[kB];
        if (!out.process(l, r, kB)) {
            idleBlocks++;
            return;
        }
        for (int i = 0; i < kB; i++) {
            TEST_ASSERT_FLOAT_WITHIN(1.5f / 32768.0f, -l[i], r[i]);  // (the tempo change rounds each side by itself)
            heard.push_back((int)lrintf(l[i] * 32768.0f));
        }
    }
    void block() {
        player.service();
        audio();
    }
    void run(int blocks) {
        for (int i = 0; i < blocks; i++) block();
    }
};

static void test_plays_the_file_in_order_then_stops_at_the_start() {
    NumberTrack track(20000);
    Rig rig;
    TEST_ASSERT_EQUAL(TrackPlayer::kEmpty, rig.player.state());
    rig.player.setSource(&track);
    TEST_ASSERT_EQUAL(TrackPlayer::kStopped, rig.player.state());
    rig.run(10);
    TEST_ASSERT_EQUAL_INT(0, (int)rig.heard.size());  // loaded, not playing: silent
    rig.player.play();
    for (int i = 0; i < 2000 && rig.player.state() == TrackPlayer::kPlaying; i++) {
        rig.block();
        // What is heard is where the player says it is.
        if (rig.heard.size() > (size_t)Rig::kFadeIn && rig.heard.size() < 20000)
            TEST_ASSERT_EQUAL_UINT32((uint32_t)rig.heard.back() + 1, rig.player.position());
    }
    rig.run(50);
    TEST_ASSERT_EQUAL(TrackPlayer::kStopped, rig.player.state());
    TEST_ASSERT_TRUE(rig.out.idle());
    TEST_ASSERT_EQUAL_UINT32(0, rig.player.position());
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
    TEST_ASSERT_TRUE(rig.heard.size() >= 20000);
    for (int j = 0; j < Rig::kFadeIn; j++) TEST_ASSERT_TRUE(rig.heard[j] <= j);  // fading in
    for (int j = Rig::kFadeIn; j < 20000; j++) TEST_ASSERT_EQUAL_INT(j, rig.heard[j]);
    for (size_t j = 20000; j < rig.heard.size(); j++) TEST_ASSERT_EQUAL_INT(0, rig.heard[j]);

    // And again from the start.
    const size_t before = rig.heard.size();
    rig.player.play();
    rig.run(100);
    TEST_ASSERT_EQUAL_INT(Rig::kFadeIn + 5, rig.heard[before + Rig::kFadeIn + 5]);
}

static void test_pause_holds_the_place() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(200);
    rig.player.pause();
    rig.run(30);
    TEST_ASSERT_EQUAL(TrackPlayer::kPaused, rig.player.state());
    TEST_ASSERT_TRUE(rig.out.idle());
    const uint32_t at = rig.player.position();
    const size_t n = rig.heard.size();
    rig.run(100);
    TEST_ASSERT_EQUAL_UINT32(at, rig.player.position());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)rig.heard.size());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, at);  // every frame taken was output: none skipped
    rig.player.play();
    rig.run(100);
    // Continues with the very next frame (the frame number is its index in what was output).
    for (size_t j = n + Rig::kFadeIn; j < rig.heard.size(); j++) TEST_ASSERT_EQUAL_INT((int)j, rig.heard[j]);
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
}

static void test_seek_while_playing_is_heard_within_60_ms() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(100);
    const size_t n = rig.heard.size();
    rig.player.seek(20000);
    rig.run(150);
    size_t first = n;
    while (first < rig.heard.size() && rig.heard[first] < 15000) first++;
    TEST_ASSERT_TRUE(first < rig.heard.size());
    TEST_ASSERT_TRUE(first - n <= TrackPlayer::kKeepFrames + StereoRing::kFade + Rig::kB);
    // Up to the join the old place goes on, without a gap.
    for (size_t j = n; j < n + TrackPlayer::kKeepFrames - Rig::kB; j++) TEST_ASSERT_EQUAL_INT((int)j, rig.heard[j]);
    // After the crossfade: the new place, frame by frame.
    const size_t clear = first + StereoRing::kFade;
    for (size_t j = clear + 1; j < rig.heard.size(); j++) TEST_ASSERT_EQUAL_INT(rig.heard[j - 1] + 1, rig.heard[j]);
    TEST_ASSERT_TRUE(rig.heard[clear] >= 20000 && rig.heard[clear] < 20000 + 2 * (int)StereoRing::kFade);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)rig.heard.back() + 1, rig.player.position());
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
}

static void test_seek_while_stopped_starts_there() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.seek(10000);
    TEST_ASSERT_EQUAL_UINT32(10000, rig.player.position());
    rig.player.play();
    rig.run(60);
    TEST_ASSERT_EQUAL_INT(10000 + Rig::kFadeIn, rig.heard[Rig::kFadeIn]);
    // Paused, moved, resumed.
    rig.player.pause();
    rig.run(30);
    rig.player.seek(500);
    TEST_ASSERT_EQUAL_UINT32(500, rig.player.position());
    const size_t n = rig.heard.size();
    rig.player.play();
    rig.run(60);
    TEST_ASSERT_EQUAL_INT(500 + Rig::kFadeIn, rig.heard[n + Rig::kFadeIn]);
}

static void test_a_loop_goes_round_without_a_gap() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    TEST_ASSERT_FALSE(rig.player.setLoop(6000, 6000 + TrackPlayer::kMinLoopFrames - 1));  // too short
    TEST_ASSERT_FALSE(rig.player.setLoop(6000, 30001));                                   // past the end
    TEST_ASSERT_FALSE(rig.player.looping());
    TEST_ASSERT_TRUE(rig.player.setLoop(6000, 20000));
    rig.player.play();
    for (int i = 0; i < 3000; i++) {
        rig.block();
        TEST_ASSERT_TRUE(rig.player.position() < 20000);
    }
    const int S = TrackPlayer::kSpliceFrames;
    int rounds = 0;
    for (size_t j = Rig::kFadeIn + 1; j + S < rig.heard.size(); j++) {
        if (rig.heard[j] == rig.heard[j - 1] + 1) continue;
        // The join: the frame before it is the last before B, then S frames blend what
        // follows B into what follows A, then the loop goes on from A + S.
        TEST_ASSERT_EQUAL_INT(19999, rig.heard[j - 1]);
        for (int k = 0; k < S; k++) {
            TEST_ASSERT_TRUE(rig.heard[j + k] > 6000 && rig.heard[j + k] <= 20000 + S);
            if (k) TEST_ASSERT_TRUE(rig.heard[j + k] <= rig.heard[j + k - 1] + 1);  // falls from B's side to A's
        }
        TEST_ASSERT_EQUAL_INT(6000 + S, rig.heard[j + S]);
        rounds++;
        j += S;
    }
    TEST_ASSERT_TRUE(rounds >= 5);  // 96000 frames, 14000 a round
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
    // Stop goes to the loop's start.
    rig.player.stop();
    rig.run(30);
    TEST_ASSERT_EQUAL_UINT32(6000, rig.player.position());
}

static void test_a_loop_set_while_playing_holds_at_once() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(100);  // by now the whole file is queued
    TEST_ASSERT_TRUE(rig.player.setLoop(2000, 14000));
    const int S = TrackPlayer::kSpliceFrames;
    size_t n = rig.heard.size();
    rig.run(1500);  // 48000 frames: four rounds
    int top = 0;
    for (size_t j = n; j < rig.heard.size(); j++) top = rig.heard[j] > top ? rig.heard[j] : top;
    TEST_ASSERT_TRUE(top >= 13999 && top <= 14000 + S);
    // Loop off in the middle of a round: plays on past B to the end.
    while (rig.player.position() < 5000 || rig.player.position() > 9000) rig.block();
    rig.player.clearLoop();
    for (int i = 0; i < 2000 && rig.player.state() == TrackPlayer::kPlaying; i++) rig.block();
    TEST_ASSERT_EQUAL(TrackPlayer::kStopped, rig.player.state());
    top = 0;
    for (size_t j = n; j < rig.heard.size(); j++) top = rig.heard[j] > top ? rig.heard[j] : top;
    TEST_ASSERT_EQUAL_INT(29999, top);
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
}

static void test_a_loop_set_just_before_its_end_still_turns() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    while (rig.heard.size() < 13000) rig.block();  // 1000 frames (21 ms) before B
    TEST_ASSERT_TRUE(rig.player.setLoop(1000, 14000));
    const size_t n = rig.heard.size();
    rig.run(300);
    int top = 0;
    for (size_t j = n; j < rig.heard.size(); j++) top = rig.heard[j] > top ? rig.heard[j] : top;
    TEST_ASSERT_TRUE(top < 14000 + 2 * (int)StereoRing::kFade + (int)TrackPlayer::kKeepFrames);
    TEST_ASSERT_TRUE(rig.player.position() < 14000);
}

static void test_the_main_loop_may_be_away() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(50);
    for (int i = 0; i < 600; i++) rig.audio();  // 0.4 s of audio without a pass of the main loop
    rig.run(50);
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
    for (size_t j = Rig::kFadeIn; j < rig.heard.size(); j++) TEST_ASSERT_EQUAL_INT((int)j, rig.heard[j]);
}

static void test_taking_the_track_away_ends_it() {
    NumberTrack track(30000), other(5000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(100);
    rig.player.setSource(&other);  // a new track while one plays: stopped, at its start
    const uint32_t readTo = track.pos;
    rig.run(30);
    TEST_ASSERT_EQUAL(TrackPlayer::kStopped, rig.player.state());
    TEST_ASSERT_TRUE(rig.out.idle());
    TEST_ASSERT_EQUAL_UINT32(readTo, track.pos);  // the old one is not read again
    const size_t n = rig.heard.size();
    rig.player.play();
    rig.run(40);
    TEST_ASSERT_EQUAL_INT(Rig::kFadeIn + 3, rig.heard[n + Rig::kFadeIn + 3]);
    rig.player.setSource(nullptr);
    rig.run(30);
    TEST_ASSERT_EQUAL(TrackPlayer::kEmpty, rig.player.state());
    TEST_ASSERT_TRUE(rig.out.idle());
    TEST_ASSERT_EQUAL_UINT32(0, rig.ring.queued());
    rig.player.play();  // nothing to play
    rig.run(10);
    TEST_ASSERT_TRUE(rig.out.idle());
}

// How far the player's place moves while `blocks` blocks are heard.
static int32_t advance(Rig& rig, int blocks) {
    const uint32_t from = rig.player.position();
    rig.run(blocks);
    return (int32_t)(rig.player.position() - from);
}

static void test_a_slower_tempo_takes_longer_and_the_place_follows() {
    NumberTrack track(24000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.setTempo(0.5f);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, rig.player.tempo());
    rig.player.play();
    rig.run(100);
    // 300 blocks are 9600 frames heard: 4800 of the track at half tempo.
    TEST_ASSERT_INT_WITHIN(300, 4800, advance(rig, 300));
    int blocks = 400;
    for (; blocks < 4000 && rig.player.state() == TrackPlayer::kPlaying; blocks++) rig.block();
    // 24000 frames at half tempo are 48000 heard, 1500 blocks (and a few to start and end).
    TEST_ASSERT_INT_WITHIN(60, 1500, blocks);
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
    TEST_ASSERT_EQUAL_UINT32(0, rig.player.position());

    // Faster: 1.5 times.
    rig.player.setTempo(1.5f);
    rig.player.play();
    rig.run(100);
    TEST_ASSERT_INT_WITHIN(450, 14400, advance(rig, 300));
    // Held to its range, and "as recorded" switches the stretcher off.
    rig.player.setTempo(3.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, rig.player.tempo());
    rig.player.setTempo(1.001f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.player.tempo());
}

static void test_a_tempo_change_while_playing_is_heard_at_once_and_keeps_the_place() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.play();
    rig.run(150);
    const uint32_t before = rig.player.position();
    rig.player.setTempo(0.6f);
    rig.run(4);
    // The place does not jump...
    TEST_ASSERT_INT_WITHIN(400, (int)before + 4 * Rig::kB, (int)rig.player.position());
    rig.run(170);  // (the 100 ms still queued at the old tempo pass)
    // ...and from then on moves at the new tempo: 9600 heard are 5760 of the track.
    TEST_ASSERT_INT_WITHIN(350, 5760, advance(rig, 300));
    // Back to the track's own tempo: frame by frame again.
    rig.player.setTempo(1.0f);
    rig.run(200);
    for (size_t j = rig.heard.size() - 500; j < rig.heard.size(); j++) TEST_ASSERT_EQUAL_INT(rig.heard[j - 1] + 1, rig.heard[j]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)rig.heard.back() + 1, rig.player.position());
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
    // Paused: the tempo changes, the place stays.
    rig.player.pause();
    rig.run(30);
    const uint32_t at = rig.player.position();
    rig.player.setTempo(0.8f);
    rig.run(30);
    TEST_ASSERT_EQUAL_UINT32(at, rig.player.position());
}

static void test_a_loop_at_a_slower_tempo() {
    NumberTrack track(30000);
    Rig rig;
    rig.player.setSource(&track);
    rig.player.setTempo(0.75f);
    TEST_ASSERT_TRUE(rig.player.setLoop(6000, 18000));
    rig.player.play();
    int rounds = 0;
    uint32_t last = 0;
    for (int i = 0; i < 3000; i++) {  // 96000 heard = 72000 of the track = 6000 + five rounds and a half
        rig.block();
        const uint32_t p = rig.player.position();
        TEST_ASSERT_TRUE(p < 18000 + 600);
        if (p + 6000 < last) rounds++;
        last = p;
    }
    TEST_ASSERT_INT_WITHIN(1, 5, rounds);
    TEST_ASSERT_EQUAL_UINT32(0, rig.out.underruns());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_plays_the_file_in_order_then_stops_at_the_start);
    RUN_TEST(test_pause_holds_the_place);
    RUN_TEST(test_seek_while_playing_is_heard_within_60_ms);
    RUN_TEST(test_seek_while_stopped_starts_there);
    RUN_TEST(test_a_loop_goes_round_without_a_gap);
    RUN_TEST(test_a_loop_set_while_playing_holds_at_once);
    RUN_TEST(test_a_loop_set_just_before_its_end_still_turns);
    RUN_TEST(test_the_main_loop_may_be_away);
    RUN_TEST(test_taking_the_track_away_ends_it);
    RUN_TEST(test_a_slower_tempo_takes_longer_and_the_place_follows);
    RUN_TEST(test_a_tempo_change_while_playing_is_heard_at_once_and_keeps_the_place);
    RUN_TEST(test_a_loop_at_a_slower_tempo);
    return UNITY_END();
}
