// Host unit tests for dsp/player/TrackInfo.h (the file kept beside a backing track, and the
// beat grid's arithmetic).
// Run with: pio test -e native
#include <unity.h>
#include <cstring>
#include <dsp/player/TrackInfo.h>

using dsp::TrackInfo;

void setUp() {}
void tearDown() {}

static void test_what_is_written_is_read_back() {
    TrackInfo a;
    a.fileBytes = 2772705;
    a.scanned = true;
    a.bpm = 97.0012f;
    a.firstBeat = 0.2134f;
    a.confidence = 0.91f;
    a.beatsPerBar = 3;
    a.tempo = 0.8f;
    a.loopFrom = 12.5f;
    a.loopTo = 20.25f;
    a.snap = false;
    char text[512];
    const int len = dsp::formatTrackInfo(a, text, sizeof(text));
    TEST_ASSERT_EQUAL_INT((int)strlen(text), len);
    TrackInfo b;
    TEST_ASSERT_TRUE(dsp::parseTrackInfo(text, b));
    TEST_ASSERT_EQUAL_UINT32(a.fileBytes, b.fileBytes);
    TEST_ASSERT_TRUE(b.scanned);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, a.bpm, b.bpm);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, a.firstBeat, b.firstBeat);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, a.confidence, b.confidence);
    TEST_ASSERT_EQUAL_INT(3, b.beatsPerBar);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.8f, b.tempo);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, b.loopFrom);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.25f, b.loopTo);
    TEST_ASSERT_FALSE(b.snap);

    // Nothing set: no bar and no loop lines, and they read back as not set.
    TrackInfo plain;
    dsp::formatTrackInfo(plain, text, sizeof(text));
    TEST_ASSERT_NULL(strstr(text, "bar "));
    TEST_ASSERT_NULL(strstr(text, "loop "));
    TEST_ASSERT_TRUE(dsp::parseTrackInfo(text, b));
    TEST_ASSERT_TRUE(b.looping);  // (matters only once there are points)
    TEST_ASSERT_FALSE(b.hasGrid());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.loopTo);
    TEST_ASSERT_TRUE(b.snap);
    // A buffer too small is not overrun.
    char tiny[20];
    TEST_ASSERT_TRUE(dsp::formatTrackInfo(a, tiny, sizeof(tiny)) < (int)sizeof(tiny));
}

static void test_other_files_and_damage() {
    TrackInfo t;
    TEST_ASSERT_FALSE(dsp::parseTrackInfo("", t));
    TEST_ASSERT_FALSE(dsp::parseTrackInfo("hello\nworld\n", t));
    TEST_ASSERT_FALSE(dsp::parseTrackInfo("track 2\nbpm 120\n", t));  // a later version
    // Unknown lines, a line without a value, Windows line ends, no final line end.
    TEST_ASSERT_TRUE(dsp::parseTrackInfo("track 1\r\nfuture 7 8 9\r\nbpm 120.5\r\njunk\r\nbeats 6", t));
    TEST_ASSERT_EQUAL_FLOAT(120.5f, t.bpm);
    TEST_ASSERT_EQUAL_INT(6, t.beatsPerBar);
    // Values out of any sense fall back.
    TEST_ASSERT_TRUE(dsp::parseTrackInfo("track 1\nbpm 9999\nbeats 0\ntempo 17\nloop 30 10\n", t));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, t.bpm);
    TEST_ASSERT_EQUAL_INT(4, t.beatsPerBar);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t.tempo);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, t.loopTo);
}

static void test_the_grid() {
    TrackInfo t;
    t.bpm = 120.0f;       // a beat every 0.5 s
    t.firstBeat = 0.25f;  // beats at 0.25, 0.75, 1.25 ...
    TEST_ASSERT_TRUE(t.hasGrid());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.25f, (float)t.beatTime(2));
    TEST_ASSERT_EQUAL_INT(0, (int)t.beatAt(0.25));
    TEST_ASSERT_EQUAL_INT(0, (int)t.beatAt(0.74));
    TEST_ASSERT_EQUAL_INT(1, (int)t.beatAt(0.75));
    TEST_ASSERT_EQUAL_INT(-1, (int)t.beatAt(0.1));
    TEST_ASSERT_EQUAL_INT(1, (int)t.nearestBeat(0.6));
    TEST_ASSERT_EQUAL_INT(0, (int)t.nearestBeat(0.45));
    // Bars of four from the first beat: bar 3 starts at beat 8.
    TEST_ASSERT_EQUAL_INT(8, (int)t.barLine(3));
    int bar, beat;
    t.barAndBeat(0, &bar, &beat);
    TEST_ASSERT_EQUAL_INT(1, bar);
    TEST_ASSERT_EQUAL_INT(1, beat);
    t.barAndBeat(7, &bar, &beat);
    TEST_ASSERT_EQUAL_INT(2, bar);
    TEST_ASSERT_EQUAL_INT(4, beat);
    // Before the first beat: bar 0 and below.
    t.barAndBeat(-1, &bar, &beat);
    TEST_ASSERT_EQUAL_INT(0, bar);
    TEST_ASSERT_EQUAL_INT(4, beat);
    t.barAndBeat(-5, &bar, &beat);
    TEST_ASSERT_EQUAL_INT(-1, bar);
    TEST_ASSERT_EQUAL_INT(4, beat);
}

static void test_a_loop_switched_off_keeps_its_points() {
    TrackInfo a;
    a.fileBytes = 1000;
    a.loopFrom = 12.5f;
    a.loopTo = 20.25f;
    a.looping = false;
    char text[512];
    dsp::formatTrackInfo(a, text, sizeof(text));
    TEST_ASSERT_NOT_NULL(strstr(text, "loop 12.500 20.250\nlooping 0\n"));
    TrackInfo b;
    TEST_ASSERT_TRUE(dsp::parseTrackInfo(text, b));
    TEST_ASSERT_FALSE(b.looping);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, b.loopFrom);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.25f, b.loopTo);
    // A file from before (no "looping" line): the loop is on. Its "bar" line is skipped.
    TrackInfo c;
    TEST_ASSERT_TRUE(dsp::parseTrackInfo("track 1\nsize 1000\nbar 3\nloop 1.000 2.000\n", c));
    TEST_ASSERT_TRUE(c.looping);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, c.loopTo);
    // Without points nothing about a loop is written.
    TrackInfo d;
    dsp::formatTrackInfo(d, text, sizeof(text));
    TEST_ASSERT_NULL(strstr(text, "loop"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_what_is_written_is_read_back);
    RUN_TEST(test_other_files_and_damage);
    RUN_TEST(test_the_grid);
    RUN_TEST(test_a_loop_switched_off_keeps_its_points);
    return UNITY_END();
}
