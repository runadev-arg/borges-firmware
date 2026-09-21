#include "BorgesBookmarkSync.h"

#include <ArduinoJson.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iterator>
#include <optional>
#include <utility>

#include "../../src/BookmarkEntry.h"
#include "BorgesCredentialStore.h"
#include "BorgesDurableQueue.h"
#include "BorgesSyncCore.h"
#include "BorgesSyncScheduler.h"

namespace borges {
namespace {

constexpr size_t MAX_PASSAGE_BYTES = 3200;
constexpr size_t MAX_NOTE_BYTES = 768;
constexpr size_t MAX_BOOK_FIELD_BYTES = 500;
constexpr size_t MAX_APPLY_PER_OPEN = 32;

struct BookContext {
  std::string filename;
  std::string title;
  std::string author;
  std::string hash;
};

uint32_t basisPoints(float percentage) {
  return static_cast<uint32_t>(std::lround(std::clamp(percentage, 0.0f, 1.0f) * 10000.0f));
}

std::string filenameOf(std::string_view path) {
  const size_t separator = path.find_last_of("/\\");
  return boundedUtf8(separator == std::string_view::npos ? path : path.substr(separator + 1), MAX_BOOK_FIELD_BYTES);
}

std::optional<BookContext> contextFor(const std::string& epubPath, const std::string& title,
                                      const std::string& author) {
  BookContext context{
      .filename = filenameOf(epubPath),
      .title = boundedUtf8(title, MAX_BOOK_FIELD_BYTES),
      .author = boundedUtf8(author, MAX_BOOK_FIELD_BYTES),
      .hash = KOReaderDocumentId::calculate(epubPath),
  };
  if (context.hash.size() != 32) return std::nullopt;
  return context;
}

std::string summaryFromText(std::string_view text, float percentage) {
  std::string summary;
  summary.reserve(76);
  bool previousSpace = true;
  for (unsigned char character : text) {
    const bool space = std::isspace(character) != 0;
    if (space) {
      if (!previousSpace && summary.size() < 76) summary.push_back(' ');
    } else if (summary.size() < 76) {
      summary.push_back(static_cast<char>(character));
    }
    previousSpace = space;
    if (summary.size() >= 76) break;
  }
  while (!summary.empty() && summary.back() == ' ') summary.pop_back();
  if (!summary.empty()) return boundedUtf8(summary, 72);
  return "Bookmark at " + std::to_string(static_cast<unsigned>(basisPoints(percentage) / 100U)) + "%";
}

ResolvedTime eventTime() {
  constexpr std::time_t MIN_TRUSTED_TIME = 1767225600;  // 2026-01-01
  const std::time_t systemTime = std::time(nullptr);
  if (systemTime >= MIN_TRUSTED_TIME) {
    return {.unixSeconds = static_cast<uint64_t>(systemTime), .precision = TimePrecision::EXACT};
  }
  return resolveWallTime(BORGES_QUEUE.checkpoint(), BORGES_QUEUE.getBootId(), millis());
}

std::string serializeEvent(const EventIdentity& identity, const BookContext& book, const BookmarkEntry& bookmark,
                           std::string_view eventType, std::string_view passage) {
  JsonDocument event;
  event["client_event_id"] = identity.id;
  event["client_sequence"] = std::to_string(identity.sequence);
  event["event_version"] = 1;
  event["event_type"] = eventType;
  event["aggregate_type"] = "annotation";
  event["aggregate_id"] = bookmark.borgesSyncId;
  JsonObject identifier = event["book_identifier"].to<JsonObject>();
  identifier["kind"] = "koreader_partial_md5";
  identifier["value"] = book.hash;
  const ResolvedTime wall = eventTime();
  event["occurred_at"] = formatRfc3339(wall.unixSeconds);
  event["time_precision"] = timePrecisionName(wall.precision);
  event["monotonic_ms"] = std::to_string(millis());

  JsonObject payload = event["payload"].to<JsonObject>();
  payload["sync_id"] = bookmark.borgesSyncId;
  payload["base_revision"] = std::to_string(bookmark.borgesRevision);
  if (eventType != "bookmark.deleted") {
    const uint32_t progress = basisPoints(bookmark.percentage);
    payload["kind"] = "bookmark";
    payload["text"] = passage;
    if (!bookmark.borgesNote.empty()) payload["note"] = boundedUtf8(bookmark.borgesNote, MAX_NOTE_BYTES);
    payload["page"] = progress;
    payload["total_pages"] = 10000;
    payload["percentage"] = static_cast<double>(progress) / 100.0;
    payload["drawer"] = "bookmark";
    if (!bookmark.xpath.empty()) payload["xpointer"] = boundedUtf8(bookmark.xpath, 512);
    JsonObject position = payload["position"].to<JsonObject>();
    position["native"] = "crosspoint";
    position["spine_index"] = bookmark.computedSpineIndex;
    position["chapter_page_count"] = bookmark.computedChapterPageCount;
    position["chapter_progress"] = bookmark.computedChapterProgress;
  }
  JsonObject bookPayload = payload["book"].to<JsonObject>();
  bookPayload["title"] = book.title.empty() ? book.filename : book.title;
  if (!book.author.empty()) bookPayload["author"] = book.author;
  bookPayload["file"] = book.filename;
  bookPayload["total_pages"] = 10000;

  std::string serialized;
  serializeJson(event, serialized);
  if (event.overflowed() || serialized.size() > DurableQueue::MAX_EVENT_BYTES) return {};
  return serialized;
}

bool enqueueUpsert(const BookContext& book, BookmarkEntry& bookmark, std::string passage) {
  const uint32_t progress = basisPoints(bookmark.percentage);
  if (bookmark.borgesSyncId.empty()) {
    bookmark.borgesSyncId = legacyBookmarkSyncId(book.hash, bookmark.xpath, progress);
  }
  passage = boundedUtf8(passage, MAX_PASSAGE_BYTES);
  if (passage.empty()) passage = summaryFromText(bookmark.summary, bookmark.percentage);
  const std::string fingerprint =
      annotationFingerprint(bookmark.borgesSyncId, passage, bookmark.borgesNote, bookmark.xpath, progress);
  if (bookmark.borgesFingerprint == fingerprint && !bookmark.borgesConflict && bookmark.borgesPendingEventId.empty()) {
    return true;
  }

  if (isUuid(bookmark.borgesPendingEventId) && bookmark.borgesPendingSequence > 0) {
    const EventIdentity pending{.id = bookmark.borgesPendingEventId, .sequence = bookmark.borgesPendingSequence};
    const std::string event = serializeEvent(pending, book, bookmark, "bookmark.upserted", passage);
    if (!event.empty() && BORGES_QUEUE.replaceUnattempted(pending, event)) {
      bookmark.borgesFingerprint = fingerprint;
      bookmark.borgesConflict = false;
      BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
      return true;
    }
  }

  const auto identity = BORGES_QUEUE.reserveIdentity();
  if (!identity) return false;
  const std::string event = serializeEvent(*identity, book, bookmark, "bookmark.upserted", passage);
  if (event.empty() || !BORGES_QUEUE.enqueue(*identity, event)) return false;
  bookmark.borgesPendingEventId = identity->id;
  bookmark.borgesPendingSequence = identity->sequence;
  bookmark.borgesFingerprint = fingerprint;
  bookmark.borgesConflict = false;
  BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
  return true;
}

bool applyAnnotation(const AnnotationInboxItem& item, std::vector<BookmarkEntry>& bookmarks, bool& changed) {
  auto existing = std::find_if(bookmarks.begin(), bookmarks.end(),
                               [&item](const BookmarkEntry& bookmark) { return bookmark.borgesSyncId == item.syncId; });
  if (item.conflict) {
    if (existing != bookmarks.end()) {
      existing->borgesConflict = true;
      existing->borgesRevision = item.revision;
      existing->borgesPendingEventId.clear();
      existing->borgesPendingSequence = 0;
      changed = true;
    }
    return true;
  }

  if (item.deleted) {
    if (existing != bookmarks.end()) {
      bookmarks.erase(existing);
      changed = true;
    }
    return true;
  }

  if (item.directive == "own") {
    if (existing != bookmarks.end()) {
      existing->borgesRevision = item.revision;
      existing->borgesPendingEventId.clear();
      existing->borgesPendingSequence = 0;
      existing->borgesConflict = false;
      changed = true;
      return true;
    }
  }
  if (item.directive != "apply_remote" && item.directive != "own") return false;

  if (existing == bookmarks.end()) {
    bookmarks.emplace_back();
    existing = std::prev(bookmarks.end());
  }
  BookmarkEntry& bookmark = *existing;
  bookmark.borgesSyncId = item.syncId;
  bookmark.borgesRevision = item.revision;
  bookmark.borgesPendingEventId.clear();
  bookmark.borgesPendingSequence = 0;
  bookmark.borgesConflict = false;
  bookmark.xpath = item.xpointer;
  bookmark.percentage = static_cast<float>(item.percentage / 100.0);
  bookmark.summary = summaryFromText(item.text, bookmark.percentage);
  bookmark.borgesNote = boundedUtf8(item.note, MAX_NOTE_BYTES);
  bookmark.computedSpineIndex = item.spineIndex;
  bookmark.computedChapterPageCount = item.chapterPageCount;
  bookmark.computedChapterProgress = item.chapterProgress;
  bookmark.borgesFingerprint =
      annotationFingerprint(item.syncId, item.text, item.note, item.xpointer, basisPoints(bookmark.percentage));
  changed = true;
  return true;
}

}  // namespace

BookmarkSync& BookmarkSync::instance() {
  static BookmarkSync sync;
  return sync;
}

bool BookmarkSync::reconcile(const std::string& epubPath, const std::string& title, const std::string& author,
                             std::vector<BookmarkEntry>& bookmarks, PersistBookmarks persist) {
  if (!BORGES_QUEUE.begin()) return false;
  const auto book = contextFor(epubPath, title, author);
  if (!book) return false;

  bool changed = false;
  uint64_t afterSequence = 0;
  std::vector<uint64_t> processedSequences;
  processedSequences.reserve(MAX_APPLY_PER_OPEN);
  for (size_t count = 0; count < MAX_APPLY_PER_OPEN; ++count) {
    const auto item = BORGES_QUEUE.nextAnnotation(book->hash, afterSequence);
    if (!item) break;
    if (!applyAnnotation(*item, bookmarks, changed)) {
      LOG_ERR("BORGES", "Unsupported annotation directive '%s'", item->directive.c_str());
      break;
    }
    afterSequence = item->serverSequence;
    processedSequences.push_back(item->serverSequence);
  }

  if (BORGES_CREDENTIALS.paired()) {
    for (BookmarkEntry& bookmark : bookmarks) {
      if (bookmark.borgesSyncId.empty()) {
        bookmark.borgesSyncId = legacyBookmarkSyncId(book->hash, bookmark.xpath, basisPoints(bookmark.percentage));
        changed = true;
      }
      if (bookmark.borgesFingerprint.empty() && enqueueUpsert(*book, bookmark, bookmark.summary)) {
        changed = true;
      }
    }
  }

  if (changed && (persist == nullptr || !persist(epubPath, bookmarks))) {
    LOG_ERR("BORGES", "Bookmark state was not persisted; retaining annotation inbox");
    return false;
  }
  for (uint64_t sequence : processedSequences) {
    if (!BORGES_QUEUE.resolveInbox(sequence)) {
      LOG_ERR("BORGES", "Could not retire annotation inbox item %llu", static_cast<unsigned long long>(sequence));
      return changed;
    }
  }
  return changed;
}

bool BookmarkSync::enqueueLocalUpsert(const std::string& epubPath, const std::string& title, const std::string& author,
                                      BookmarkEntry& bookmark, const std::string& visiblePassage) {
  if (!BORGES_CREDENTIALS.paired()) return true;
  if (!BORGES_QUEUE.begin()) return false;
  const auto book = contextFor(epubPath, title, author);
  return book && enqueueUpsert(*book, bookmark, visiblePassage);
}

bool BookmarkSync::enqueueLocalDelete(const std::string& epubPath, const std::string& title, const std::string& author,
                                      const BookmarkEntry& bookmark) {
  if (!BORGES_CREDENTIALS.paired() || bookmark.borgesSyncId.empty()) return true;
  if (!BORGES_QUEUE.begin()) return false;
  const auto book = contextFor(epubPath, title, author);
  if (!book) return false;

  if (isUuid(bookmark.borgesPendingEventId) && bookmark.borgesPendingSequence > 0) {
    const EventIdentity pending{.id = bookmark.borgesPendingEventId, .sequence = bookmark.borgesPendingSequence};
    const std::string event = serializeEvent(pending, *book, bookmark, "bookmark.deleted", {});
    if (!event.empty() && BORGES_QUEUE.replaceUnattempted(pending, event)) {
      BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
      return true;
    }
  }

  const auto identity = BORGES_QUEUE.reserveIdentity();
  if (!identity) return false;
  const std::string event = serializeEvent(*identity, *book, bookmark, "bookmark.deleted", {});
  if (event.empty() || !BORGES_QUEUE.enqueue(*identity, event)) return false;
  BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
  return true;
}

}  // namespace borges
