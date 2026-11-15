// Host unit tests for dsp/player/TrackIndex.h (the list of the backing tracks in their
// folders, the search over it, and a browser's place in it).
// Run with: pio test -e native
#include <unity.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <dsp/player/TrackIndex.h>

using dsp::TrackBrowser;
using dsp::TrackIndex;

void setUp() {}
void tearDown() {}

// A card: for each folder's path (the root: ""), its entries in the order the card gives
// them. A name ending in '/' is a folder.
typedef std::map<std::string, std::vector<std::string>> Card;

static Card theCard() {
    Card c;
    c[""] = {"zebra.mp3", "Rock", "alpha.wav", "Blues", "Mango_Tree.mp3"};
    c["Rock"] = {"Thunder Road.mp3", "slow", "AC_DC - Back.mp3", "back in time.wav"};
    c["Rock/slow"] = {"Ballad in E.mp3"};
    c["Blues"] = {"Slow Blues in A.mp3", "shuffle in e.wav", "Fast Shuffle.mp3"};
    return c;
}

static bool isFolderName(const Card& c, const std::string& dir, const std::string& name) {
    return c.count(dir.empty() ? name : dir + "/" + name) > 0;
}

// Reads the card into the index the way the firmware does; at most `folders` folders.
static void build(TrackIndex& ix, const Card& c, int folders = 1000) {
    ix.begin();
    while (!ix.done() && folders-- > 0) {
        char path[TrackIndex::kPathMax];
        TEST_ASSERT_TRUE(ix.pathOf(ix.reading(), path, sizeof(path)));
        const auto it = c.find(path);
        if (it != c.end())
            for (const std::string& n : it->second) ix.add(n.c_str(), isFolderName(c, path, n));
        ix.endFolder();
    }
}

struct Store {
    std::vector<TrackIndex::Entry> entries;
    std::vector<char> pool;
    TrackIndex ix;
    explicit Store(int maxEntries = 256, int poolBytes = 8192)
        : entries(maxEntries), pool(poolBytes), ix({entries.data(), maxEntries, pool.data(), (uint32_t)poolBytes}) {}
};

static std::string rowsOf(const TrackIndex& ix, int folder) {
    std::string s;
    for (int i = 0; i < ix.rows(folder); i++) {
        if (i) s += "|";
        s += ix.name(ix.row(folder, i));
        if (ix.isFolder(ix.row(folder, i))) s += "/";
    }
    return s;
}

static std::string pathOf(const TrackIndex& ix, int id) {
    char path[TrackIndex::kPathMax];
    TEST_ASSERT_TRUE(ix.pathOf(id, path, sizeof(path)));
    return path;
}

static void test_folders_come_first_then_names_whatever_the_case() {
    Store s;
    build(s.ix, theCard());
    TEST_ASSERT_TRUE(s.ix.done());
    TEST_ASSERT_EQUAL_INT(10, s.ix.files());
    TEST_ASSERT_EQUAL_INT(14, s.ix.entries());  // the root, 3 folders, 10 files
    TEST_ASSERT_EQUAL_STRING("Blues/|Rock/|alpha.wav|Mango_Tree.mp3|zebra.mp3", rowsOf(s.ix, 0).c_str());
    const int rock = s.ix.findPath("Rock");
    TEST_ASSERT_TRUE(s.ix.isFolder(rock));
    TEST_ASSERT_EQUAL_STRING("slow/|AC_DC - Back.mp3|back in time.wav|Thunder Road.mp3", rowsOf(s.ix, rock).c_str());
    TEST_ASSERT_EQUAL_STRING("Fast Shuffle.mp3|shuffle in e.wav|Slow Blues in A.mp3", rowsOf(s.ix, s.ix.findPath("blues")).c_str());
    TEST_ASSERT_EQUAL_INT(0, s.ix.skipped());
    TEST_ASSERT_FALSE(s.ix.full());
}

static void test_paths_go_both_ways() {
    Store s;
    build(s.ix, theCard());
    TEST_ASSERT_EQUAL_STRING("", pathOf(s.ix, 0).c_str());
    const int ballad = s.ix.findPath("Rock/slow/Ballad in E.mp3");
    TEST_ASSERT_TRUE(ballad > 0);
    TEST_ASSERT_FALSE(s.ix.isFolder(ballad));
    TEST_ASSERT_EQUAL_STRING("Rock/slow/Ballad in E.mp3", pathOf(s.ix, ballad).c_str());
    TEST_ASSERT_EQUAL_STRING("slow", s.ix.name(s.ix.parent(ballad)));
    // Whatever the case, with or without a slash in front; every entry finds itself.
    TEST_ASSERT_EQUAL_INT(ballad, s.ix.findPath("/rock/SLOW/ballad in e.MP3"));
    for (int id = 0; id < s.ix.entries(); id++) TEST_ASSERT_EQUAL_INT(id, s.ix.findPath(pathOf(s.ix, id).c_str()));
    TEST_ASSERT_EQUAL_INT(-1, s.ix.findPath("Rock/none.mp3"));
    TEST_ASSERT_EQUAL_INT(-1, s.ix.findPath("Roc"));
    TEST_ASSERT_EQUAL_INT(-1, s.ix.findPath("zebra.mp3/x"));
    // A buffer too small says so.
    char small[8];
    TEST_ASSERT_FALSE(s.ix.pathOf(ballad, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    // By name alone, in any folder.
    TEST_ASSERT_EQUAL_INT(ballad, s.ix.findName("ballad in e.mp3"));
    TEST_ASSERT_EQUAL_INT(-1, s.ix.findName("slow"));  // a folder is no track
}

static void test_a_half_built_index_can_be_used() {
    Store s;
    build(s.ix, theCard(), 1);  // only the root has been read
    TEST_ASSERT_FALSE(s.ix.done());
    TEST_ASSERT_EQUAL_STRING("Blues", s.ix.name(s.ix.reading()));
    TEST_ASSERT_EQUAL_INT(3, s.ix.files());
    TEST_ASSERT_EQUAL_STRING("Blues/|Rock/|alpha.wav|Mango_Tree.mp3|zebra.mp3", rowsOf(s.ix, 0).c_str());
    TEST_ASSERT_EQUAL_INT(0, s.ix.rows(s.ix.findPath("Rock")));  // not read yet: empty
    uint16_t hits[8];
    TEST_ASSERT_EQUAL_INT(1, s.ix.search("mango", hits, 8, nullptr));
    // A folder that cannot be read (gone from the card) is empty, and the rest goes on.
    Card c = theCard();
    c.erase("Blues");
    c[""] = {"Rock", "Blues"};
    Store t;
    t.ix.begin();
    t.ix.add("Rock", true);
    t.ix.add("Blues", true);
    t.ix.endFolder();
    TEST_ASSERT_EQUAL_STRING("Blues", t.ix.name(t.ix.reading()));
    t.ix.endFolder();
    TEST_ASSERT_EQUAL_STRING("Rock", t.ix.name(t.ix.reading()));
    t.ix.add("a.mp3", false);
    t.ix.endFolder();
    TEST_ASSERT_TRUE(t.ix.done());
    TEST_ASSERT_FALSE(t.ix.add("late.mp3", false));
    TEST_ASSERT_EQUAL_STRING("Rock/a.mp3", pathOf(t.ix, t.ix.findName("a.mp3")).c_str());
}

static void test_what_does_not_fit_is_counted() {
    // A name too long, a path too long, a folder too deep.
    Store s;
    s.ix.begin();
    const std::string longName(TrackIndex::kNameMax, 'n');
    TEST_ASSERT_FALSE(s.ix.add(longName.c_str(), false));
    TEST_ASSERT_TRUE(s.ix.add(longName.substr(1).c_str(), false));
    TEST_ASSERT_FALSE(s.ix.add("", false));
    TEST_ASSERT_EQUAL_INT(2, s.ix.skipped());
    const std::string part(100, 'p');
    TEST_ASSERT_TRUE(s.ix.add(part.c_str(), true));
    s.ix.endFolder();  // now in "ppp…" (100)
    TEST_ASSERT_TRUE(s.ix.add(part.c_str(), true));
    s.ix.endFolder();  // now in "ppp…/ppp…" (201)
    TEST_ASSERT_FALSE(s.ix.add(part.c_str(), false));           // 302 with its slash
    TEST_ASSERT_TRUE(s.ix.add(std::string(53, 'x').c_str(), false));   // 255: the longest
    TEST_ASSERT_FALSE(s.ix.add(std::string(54, 'x').c_str(), false));
    TEST_ASSERT_EQUAL_INT(4, s.ix.skipped());
    TEST_ASSERT_EQUAL_INT(255, (int)pathOf(s.ix, s.ix.findName(std::string(53, 'x').c_str())).size());

    Store d;
    d.ix.begin();
    for (int level = 1; level <= TrackIndex::kMaxDepth; level++) {
        TEST_ASSERT_TRUE(d.ix.add("d", true));
        d.ix.endFolder();
    }
    TEST_ASSERT_FALSE(d.ix.add("d", true));  // a ninth level
    TEST_ASSERT_TRUE(d.ix.add("deep.mp3", false));
    d.ix.endFolder();
    TEST_ASSERT_TRUE(d.ix.done());
    TEST_ASSERT_EQUAL_INT(1, d.ix.skipped());
    TEST_ASSERT_EQUAL_STRING("d/d/d/d/d/d/d/d/deep.mp3", pathOf(d.ix, d.ix.findName("deep.mp3")).c_str());

    // Full: no more entries, or no more room for names. What is in stays good.
    Store e(4, 8192);
    e.ix.begin();
    TEST_ASSERT_TRUE(e.ix.add("c.mp3", false));
    TEST_ASSERT_TRUE(e.ix.add("b.mp3", false));
    TEST_ASSERT_TRUE(e.ix.add("a.mp3", false));
    TEST_ASSERT_FALSE(e.ix.full());
    TEST_ASSERT_FALSE(e.ix.add("d.mp3", false));
    TEST_ASSERT_TRUE(e.ix.full());
    e.ix.endFolder();
    TEST_ASSERT_EQUAL_STRING("a.mp3|b.mp3|c.mp3", rowsOf(e.ix, 0).c_str());
    Store p(64, 14);  // the root's empty name, then 13 bytes
    p.ix.begin();
    TEST_ASSERT_TRUE(p.ix.add("12345.mp3", false));  // 10 with its end
    TEST_ASSERT_FALSE(p.ix.add("abc", true));         // 4: one too many
    TEST_ASSERT_TRUE(p.ix.full());
    TEST_ASSERT_TRUE(p.ix.add("ab", true));
    // begin() starts over.
    p.ix.begin();
    TEST_ASSERT_FALSE(p.ix.full());
    TEST_ASSERT_EQUAL_INT(1, p.ix.entries());
    TEST_ASSERT_EQUAL_INT(0, p.ix.files());
}

static std::string hitsOf(const TrackIndex& ix, const char* text, int max = 32, bool* more = nullptr) {
    std::vector<uint16_t> hits(max);
    const int n = ix.search(text, hits.data(), max, more);
    std::string s;
    for (int i = 0; i < n; i++) {
        if (i) s += "|";
        s += ix.name(hits[i]);
    }
    return s;
}

static void test_the_search() {
    Store s;
    build(s.ix, theCard());
    // Whatever the case; only files are hits; in the index's order.
    TEST_ASSERT_EQUAL_STRING("zebra.mp3", hitsOf(s.ix, "ZEB").c_str());
    TEST_ASSERT_EQUAL_STRING("Fast Shuffle.mp3|shuffle in e.wav", hitsOf(s.ix, "shuffle").c_str());
    // Every word must be there, in any order.
    TEST_ASSERT_EQUAL_STRING("shuffle in e.wav", hitsOf(s.ix, "e shuffle in").c_str());
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, "shuffle zebra").c_str());
    // '_' is a space, in the name and in the text.
    TEST_ASSERT_EQUAL_STRING("Mango_Tree.mp3", hitsOf(s.ix, "mango tree").c_str());
    TEST_ASSERT_EQUAL_STRING("Mango_Tree.mp3", hitsOf(s.ix, "Mango_Tree").c_str());
    TEST_ASSERT_EQUAL_STRING("AC_DC - Back.mp3", hitsOf(s.ix, "ac dc").c_str());
    // A folder's name counts for what is in it, at any depth.
    TEST_ASSERT_EQUAL_STRING("Fast Shuffle.mp3|shuffle in e.wav|Slow Blues in A.mp3", hitsOf(s.ix, "blues").c_str());
    TEST_ASSERT_EQUAL_STRING("AC_DC - Back.mp3|back in time.wav", hitsOf(s.ix, "rock back").c_str());
    TEST_ASSERT_EQUAL_STRING("Slow Blues in A.mp3|Ballad in E.mp3", hitsOf(s.ix, "slow").c_str());
    // Nothing to look for, nothing found.
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, "").c_str());
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, "  _ ").c_str());
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, "qqq").c_str());
    // More hits than there is room for.
    bool more = false;
    TEST_ASSERT_EQUAL_STRING("alpha.wav|Mango_Tree.mp3", hitsOf(s.ix, "a", 2, &more).c_str());
    TEST_ASSERT_TRUE(more);
    hitsOf(s.ix, "mp3", 7, &more);
    TEST_ASSERT_FALSE(more);  // exactly 7 of the 10 are .mp3
    // Many words, and a word longer than any name: no harm.
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, "a b c d e f g h i j k l").c_str());
    TEST_ASSERT_EQUAL_STRING("", hitsOf(s.ix, std::string(400, 'z').c_str()).c_str());
}

static void test_a_browser_moves_through_the_folders() {
    Store s;
    build(s.ix, theCard());
    uint16_t hits[TrackBrowser::kMaxHits];
    TrackBrowser b(s.ix, hits);
    TEST_ASSERT_EQUAL_INT(0, b.folder());
    TEST_ASSERT_EQUAL_INT(5, b.rows());
    TEST_ASSERT_FALSE(b.up());  // the root
    TEST_ASSERT_EQUAL_INT(-1, b.row(5));
    TEST_ASSERT_EQUAL_INT(-1, b.enter(9));
    // Into Rock, into slow, a track there.
    TEST_ASSERT_EQUAL_INT(-1, b.enter(1));
    TEST_ASSERT_EQUAL_STRING("Rock", s.ix.name(b.folder()));
    TEST_ASSERT_EQUAL_INT(4, b.rows());
    TEST_ASSERT_EQUAL_INT(-1, b.enter(0));
    TEST_ASSERT_EQUAL_STRING("slow", s.ix.name(b.folder()));
    const int ballad = b.enter(0);
    TEST_ASSERT_EQUAL_STRING("Ballad in E.mp3", s.ix.name(ballad));
    TEST_ASSERT_EQUAL_STRING("slow", s.ix.name(b.folder()));  // a track is not gone into
    TEST_ASSERT_EQUAL_INT(0, b.rowOf(ballad));
    TEST_ASSERT_TRUE(b.up());
    TEST_ASSERT_EQUAL_STRING("Rock", s.ix.name(b.folder()));
    TEST_ASSERT_EQUAL_INT(-1, b.rowOf(ballad));
    TEST_ASSERT_TRUE(b.up());
    TEST_ASSERT_FALSE(b.up());

    // A search from inside a folder: the hits, a tap on one, and back to that folder.
    b.enter(0);  // Blues
    b.find("slow");
    TEST_ASSERT_TRUE(b.searching());
    TEST_ASSERT_EQUAL_STRING("slow", b.text());
    TEST_ASSERT_EQUAL_INT(2, b.rows());
    TEST_ASSERT_FALSE(b.more());
    TEST_ASSERT_EQUAL_INT(ballad, b.enter(1));
    TEST_ASSERT_TRUE(b.searching());
    TEST_ASSERT_EQUAL_INT(1, b.rowOf(ballad));
    TEST_ASSERT_TRUE(b.up());
    TEST_ASSERT_FALSE(b.searching());
    TEST_ASSERT_EQUAL_STRING("Blues", s.ix.name(b.folder()));
    b.find("qqq");
    TEST_ASSERT_EQUAL_INT(0, b.rows());
    TEST_ASSERT_EQUAL_INT(-1, b.enter(0));
    // A text longer than is kept is cut, not run over.
    b.find(std::string(100, 'a').c_str());
    TEST_ASSERT_EQUAL_INT(TrackBrowser::kTextMax - 1, (int)strlen(b.text()));

    // To where a track is: its folder, and its row there.
    TEST_ASSERT_EQUAL_INT(0, b.show(ballad));
    TEST_ASSERT_FALSE(b.searching());
    TEST_ASSERT_EQUAL_STRING("slow", s.ix.name(b.folder()));
    TEST_ASSERT_EQUAL_INT(4, b.show(s.ix.findPath("zebra.mp3")));
    TEST_ASSERT_EQUAL_INT(0, b.folder());
    TEST_ASSERT_EQUAL_INT(-1, b.show(-1));
    TEST_ASSERT_EQUAL_INT(0, b.folder());
    b.enter(1);
    b.reset();
    TEST_ASSERT_EQUAL_INT(0, b.folder());
}

static void test_thousands_of_tracks() {
    // 40 folders of 150 files each, given in reverse order.
    Card c;
    for (int f = 39; f >= 0; f--) {
        char folder[32];
        snprintf(folder, sizeof(folder), "Artist %02d", f);
        c[""].push_back(folder);
        for (int t = 149; t >= 0; t--) {
            char name[64];
            snprintf(name, sizeof(name), "Song number %03d of %02d.mp3", t, f);
            c[folder].push_back(name);
        }
    }
    Store s(8192, 448 * 1024);
    build(s.ix, c);
    TEST_ASSERT_TRUE(s.ix.done());
    TEST_ASSERT_FALSE(s.ix.full());
    TEST_ASSERT_EQUAL_INT(6000, s.ix.files());
    const int a17 = s.ix.findPath("Artist 17");
    TEST_ASSERT_EQUAL_INT(150, s.ix.rows(a17));
    TEST_ASSERT_EQUAL_STRING("Song number 000 of 17.mp3", s.ix.name(s.ix.row(a17, 0)));
    TEST_ASSERT_EQUAL_STRING("Song number 149 of 17.mp3", s.ix.name(s.ix.row(a17, 149)));
    uint16_t hits[TrackBrowser::kMaxHits];
    TrackBrowser b(s.ix, hits);
    b.find("artist 17 number 04");  // "17" and "04" are also in "017 of 04"
    TEST_ASSERT_EQUAL_INT(14, b.rows());
    TEST_ASSERT_EQUAL_STRING("Artist 04/Song number 017 of 04.mp3", pathOf(s.ix, b.row(0)).c_str());
    TEST_ASSERT_EQUAL_STRING("Artist 17/Song number 004 of 17.mp3", pathOf(s.ix, b.row(2)).c_str());
    b.find("song");
    TEST_ASSERT_EQUAL_INT(TrackBrowser::kMaxHits, b.rows());
    TEST_ASSERT_TRUE(b.more());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_folders_come_first_then_names_whatever_the_case);
    RUN_TEST(test_paths_go_both_ways);
    RUN_TEST(test_a_half_built_index_can_be_used);
    RUN_TEST(test_what_does_not_fit_is_counted);
    RUN_TEST(test_the_search);
    RUN_TEST(test_a_browser_moves_through_the_folders);
    RUN_TEST(test_thousands_of_tracks);
    return UNITY_END();
}
