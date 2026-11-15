// TrackIndex.cpp - see TrackIndex.h.
#include "TrackIndex.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

namespace dsp {

void TrackIndex::begin() {
  n_ = 0;
  files_ = 0;
  poolUsed_ = 0;
  skipped_ = 0;
  full_ = false;
  reading_ = -1;
  runStart_ = 1;
  readingPathLen_ = 0;
  if (m_.maxEntries < 1 || m_.poolBytes < 1) return;
  // The root: a folder without a name.
  m_.pool[0] = 0;
  poolUsed_ = 1;
  m_.entries[0] = Entry{0, 0, 1, 0, 1, 0};
  n_ = 1;
  reading_ = 0;
}

bool TrackIndex::add(const char* name, bool folder) {
  if (reading_ < 0) return false;
  const size_t len = strlen(name);
  const int depth = m_.entries[reading_].depth + 1;
  // The path's length: the folder's, a slash unless that is the root, the name.
  const size_t pathLen = (size_t)readingPathLen_ + (readingPathLen_ ? 1 : 0) + len;
  if (len == 0 || len >= (size_t)kNameMax || pathLen >= (size_t)kPathMax || (folder && depth > kMaxDepth)) {
    skipped_++;
    return false;
  }
  if (n_ >= m_.maxEntries || n_ >= 65535 || poolUsed_ + len + 1 > m_.poolBytes) {
    full_ = true;
    return false;
  }
  memcpy(m_.pool + poolUsed_, name, len + 1);
  m_.entries[n_] = Entry{poolUsed_, (uint16_t)reading_, 0, 0, (uint8_t)(folder ? 1 : 0), (uint8_t)depth};
  poolUsed_ += (uint32_t)len + 1;
  n_++;
  if (!folder) files_++;
  return true;
}

void TrackIndex::endFolder() {
  if (reading_ < 0) return;
  const char* pool = m_.pool;
  std::sort(m_.entries + runStart_, m_.entries + n_, [pool](const Entry& a, const Entry& b) {
    if (a.folder != b.folder) return a.folder > b.folder;
    const int c = strcasecmp(pool + a.name, pool + b.name);
    return c ? c < 0 : strcmp(pool + a.name, pool + b.name) < 0;
  });
  m_.entries[reading_].first = (uint16_t)runStart_;
  m_.entries[reading_].count = (uint16_t)(n_ - runStart_);
  runStart_ = n_;
  // The next folder: every folder not read yet has a higher number than this one.
  int next = reading_ + 1;
  while (next < n_ && !m_.entries[next].folder) next++;
  reading_ = next < n_ ? next : -1;
  if (reading_ >= 0) {
    char path[kPathMax];
    readingPathLen_ = pathOf(reading_, path, sizeof(path)) ? (int)strlen(path) : kPathMax;
  }
}

bool TrackIndex::pathOf(int id, char* buf, int n) const {
  if (n < 1) return false;
  buf[0] = 0;
  if (!valid(id)) return false;
  // From the entry up to the root, then written from the root down.
  int chain[kMaxDepth + 2], depth = 0;
  for (int at = id; at != 0 && depth < kMaxDepth + 2; at = m_.entries[at].parent) chain[depth++] = at;
  int used = 0;
  while (depth > 0) {
    const char* part = name(chain[--depth]);
    const int len = (int)strlen(part);
    if (used + len + 2 > n) {
      buf[0] = 0;
      return false;
    }
    if (used) buf[used++] = '/';
    memcpy(buf + used, part, len + 1);
    used += len;
  }
  return true;
}

int TrackIndex::findPath(const char* path) const {
  int at = 0;
  while (*path == '/') path++;
  while (*path) {
    const char* end = strchr(path, '/');
    const size_t len = end ? (size_t)(end - path) : strlen(path);
    int found = -1;
    for (int i = 0; i < rows(at) && found < 0; i++) {
      const char* nm = name(row(at, i));
      if (strlen(nm) == len && !strncasecmp(nm, path, len)) found = row(at, i);
    }
    if (found < 0) return -1;
    at = found;
    path += len;
    while (*path == '/') path++;
  }
  return at;
}

int TrackIndex::findName(const char* nm) const {
  for (int i = 1; i < n_; i++)
    if (!m_.entries[i].folder && !strcasecmp(name(i), nm)) return i;
  return -1;
}

// A letter of a name as the search sees it.
static inline char folded(char c) { return c == '_' ? ' ' : (char)tolower((unsigned char)c); }

static bool contains(const char* s, const char* word, int len) {
  for (; *s; s++) {
    int k = 0;
    while (k < len && s[k] && folded(s[k]) == word[k]) k++;
    if (k == len) return true;
  }
  return false;
}

bool TrackIndex::wordIn(int id, const char* word, int len) const {
  for (int at = id; at != 0; at = m_.entries[at].parent)
    if (contains(name(at), word, len)) return true;
  return false;
}

int TrackIndex::search(const char* text, uint16_t* hits, int max, bool* more) const {
  if (more) *more = false;
  // The words, folded the way the names are.
  char buf[kNameMax];
  const char* words[kMaxWords];
  int lens[kMaxWords], numWords = 0, used = 0;
  for (const char* p = text; *p && numWords < kMaxWords;) {
    while (*p && folded(*p) == ' ') p++;
    if (!*p) break;
    const int start = used;
    while (*p && folded(*p) != ' ' && used < kNameMax - 1) buf[used++] = folded(*p++);
    while (*p && folded(*p) != ' ') p++;  // a word too long: its start is what counts
    if (used == start) break;             // no room left
    words[numWords] = buf + start;
    lens[numWords++] = used - start;
  }
  if (!numWords) return 0;
  int found = 0;
  for (int i = 1; i < n_; i++) {
    if (m_.entries[i].folder) continue;
    bool all = true;
    for (int w = 0; w < numWords && all; w++) all = wordIn(i, words[w], lens[w]);
    if (!all) continue;
    if (found >= max) {
      if (more) *more = true;
      break;
    }
    hits[found++] = (uint16_t)i;
  }
  return found;
}

// ---- TrackBrowser ----

void TrackBrowser::reset() {
  folder_ = 0;
  searching_ = false;
  more_ = false;
  numHits_ = 0;
  text_[0] = 0;
}

int TrackBrowser::row(int i) const {
  if (!searching_) return ix_.row(folder_, i);
  return i >= 0 && i < numHits_ ? hits_[i] : -1;
}

int TrackBrowser::rowOf(int id) const {
  if (!searching_) {
    const int i = id - ix_.row(folder_, 0);
    return ix_.rows(folder_) > 0 && i >= 0 && i < ix_.rows(folder_) ? i : -1;
  }
  for (int i = 0; i < numHits_; i++)
    if (hits_[i] == id) return i;
  return -1;
}

int TrackBrowser::enter(int i) {
  const int id = row(i);
  if (id < 0) return -1;
  if (!ix_.isFolder(id)) return id;
  folder_ = id;
  searching_ = false;
  return -1;
}

bool TrackBrowser::up() {
  if (searching_) {
    searching_ = false;
    return true;
  }
  if (folder_ == 0) return false;
  folder_ = ix_.parent(folder_);
  return true;
}

void TrackBrowser::find(const char* text) {
  strncpy(text_, text, kTextMax - 1);
  text_[kTextMax - 1] = 0;
  numHits_ = ix_.search(text_, hits_, kMaxHits, &more_);
  searching_ = true;
}

int TrackBrowser::show(int id) {
  searching_ = false;
  folder_ = ix_.valid(id) ? ix_.parent(id) : 0;
  return rowOf(id);
}

}  // namespace dsp
