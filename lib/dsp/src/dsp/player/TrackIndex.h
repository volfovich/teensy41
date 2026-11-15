// TrackIndex.h - the list of the backing tracks: every folder and audio file below the
// tracks' folder on the card, kept in memory handed in by the caller (the PSRAM on the
// unit), so that folders can be shown, paged through and searched without the card.
//
// Built breadth-first, a folder at a time: the caller reads one folder of the card from
// its first entry to its last, add() for each, then endFolder(), which sorts what was
// added (folders first, then by name, whatever the case) and says which folder is to be
// read next. So only one folder is open on the card at a time, each folder's rows are
// one sorted run, and the caller can do the reading a few entries at a time. While the
// index is being built, what has been read so far can be used: a folder not read yet is
// simply empty.
//
// An entry is known by its number (id); 0 is the root. The numbers hold until begin().
//
// TrackBrowser is a place in the index: a folder, or the hits of a search. The touch
// screen and the shell each have one.
//
// Portable; background only.
#pragma once

#include <stdint.h>

namespace dsp {

class TrackIndex {
 public:
  static const int kNameMax = 128;  // a name, with its end
  static const int kPathMax = 256;  // a path from the root ("Rock/Slow/Song.mp3"), with its end
  static const int kMaxDepth = 8;   // folders below the root
  static const int kMaxWords = 8;   // of a search text

  struct Entry {
    uint32_t name;    // where its name is in the pool
    uint16_t parent;  // the folder it is in
    uint16_t first;   // a folder's rows: first .. first + count - 1
    uint16_t count;
    uint8_t folder;
    uint8_t depth;  // the root: 0
  };
  struct Memory {
    Entry* entries;
    int maxEntries;  // 65535 at most
    char* pool;
    uint32_t poolBytes;
  };

  // Nothing is in it, and the memory is not touched, until begin().
  explicit TrackIndex(const Memory& m) : m_(m) {}

  // ---- Building ----
  // Empty: only the root, which is the folder to read first.
  void begin();
  // The folder whose entries are to be given next; -1: all have been read.
  int reading() const { return reading_; }
  bool done() const { return reading_ < 0; }
  // An entry of the folder being read. False if it was not taken: its name or path is
  // too long, a folder lies too deep (both counted in skipped()), or the index is full.
  bool add(const char* name, bool folder);
  // The folder being read has no more entries (or could not be read).
  void endFolder();
  int skipped() const { return skipped_; }
  bool full() const { return full_; }

  // ---- What is in it ----
  int entries() const { return n_; }  // with the root
  int files() const { return files_; }
  bool valid(int id) const { return id >= 0 && id < n_; }
  bool isFolder(int id) const { return valid(id) && m_.entries[id].folder; }
  const char* name(int id) const { return valid(id) ? m_.pool + m_.entries[id].name : ""; }
  int parent(int id) const { return valid(id) ? m_.entries[id].parent : 0; }
  // A folder's rows, sorted; row() gives the id, -1 if there is no such row.
  int rows(int folder) const { return isFolder(folder) ? m_.entries[folder].count : 0; }
  int row(int folder, int i) const { return i >= 0 && i < rows(folder) ? m_.entries[folder].first + i : -1; }
  // The path from the root, without a leading slash; the root's is "". False if n is too
  // small.
  bool pathOf(int id, char* buf, int n) const;
  // The entry at that path (as pathOf() gives it, whatever the case); -1 if there is none.
  int findPath(const char* path) const;
  // The first file with exactly that name (whatever the case), in any folder; -1: none.
  int findName(const char* name) const;
  // The files that have every word of the text in their name or in the name of a folder
  // they are in. The case does not count, and '_' is a space. Returns how many were put
  // in hits (max at most); *more: there were more than that.
  int search(const char* text, uint16_t* hits, int max, bool* more) const;

 private:
  bool wordIn(int id, const char* word, int len) const;

  Memory m_;
  int n_ = 0;
  int files_ = 0;
  uint32_t poolUsed_ = 0;
  int reading_ = -1;
  int runStart_ = 1;
  int readingPathLen_ = 0;
  int skipped_ = 0;
  bool full_ = false;
};

class TrackBrowser {
 public:
  static const int kMaxHits = 256;
  static const int kTextMax = 32;

  // hits: room for kMaxHits ids.
  TrackBrowser(const TrackIndex& index, uint16_t* hits) : ix_(index), hits_(hits) {}

  // To the root. Call when the index is built anew: the ids it held mean nothing then.
  void reset();
  // The rows shown: a folder's, or the search's hits.
  int rows() const { return searching_ ? numHits_ : ix_.rows(folder_); }
  int row(int i) const;  // the id; -1: no such row
  // The row that shows this entry; -1 if it is not among them.
  int rowOf(int id) const;
  int folder() const { return folder_; }
  bool searching() const { return searching_; }
  const char* text() const { return text_; }  // what was searched for
  bool more() const { return more_; }         // there were more hits than are shown
  // A tap on row i. A folder: goes into it, returns -1. A track: returns its id and
  // stays (a search's hits stay shown).
  int enter(int i);
  // One step back: from the hits to the folder they were asked for in, from a folder to
  // the one it is in. False at the root.
  bool up();
  void find(const char* text);
  // Goes to the folder that holds this entry (ends a search); returns the entry's row.
  int show(int id);

 private:
  const TrackIndex& ix_;
  uint16_t* hits_;
  int folder_ = 0;
  bool searching_ = false;
  bool more_ = false;
  int numHits_ = 0;
  char text_[kTextMax] = "";
};

}  // namespace dsp
