#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();

  // Read/write go through PersistableStoreBase so the JSON parser and
  // serializer stay instantiated once, in PersistableStore.cpp.
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.totoSyncId = obj["toto_id"] | "";
    bookmark.totoNote = obj["toto_note"] | "";
    bookmark.totoFingerprint = obj["toto_fp"] | "";
    bookmark.totoPendingEventId = obj["toto_event"] | "";
    bookmark.totoRevision = obj["toto_rev"] | static_cast<uint64_t>(0);
    bookmark.totoPendingSequence = obj["toto_seq"] | static_cast<uint64_t>(0);
    bookmark.totoConflict = obj["toto_conflict"] | false;
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    if (!bookmark.totoSyncId.empty()) obj["toto_id"] = bookmark.totoSyncId;
    if (!bookmark.totoNote.empty()) obj["toto_note"] = bookmark.totoNote;
    if (!bookmark.totoFingerprint.empty()) obj["toto_fp"] = bookmark.totoFingerprint;
    if (!bookmark.totoPendingEventId.empty()) obj["toto_event"] = bookmark.totoPendingEventId;
    if (bookmark.totoRevision > 0) obj["toto_rev"] = bookmark.totoRevision;
    if (bookmark.totoPendingSequence > 0) obj["toto_seq"] = bookmark.totoPendingSequence;
    if (bookmark.totoConflict) obj["toto_conflict"] = true;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
  }

  // writeDocToFile ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
