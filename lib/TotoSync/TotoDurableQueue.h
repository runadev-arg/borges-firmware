#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "TotoResumeFlow.h"
#include "TotoSyncCore.h"

namespace toto {

struct EventIdentity {
  std::string id;
  uint64_t sequence = 0;
};

struct PendingEvent {
  std::string id;
  uint64_t sequence = 0;
  std::string body;
  bool attempted = false;
};

struct ProgressInboxItem {
  uint64_t serverSequence = 0;
  std::string directive;
  std::string suggestionId;
  std::string bookHash;
  double percentage = 0;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  std::string xpointer;
  bool accepted = false;
  // Who left this position and when. A reader deciding whether to jump needs
  // to know it came from their Kobo, not from "the server".
  std::string eventId;
  std::string originName;
  std::string originPlatform;
  std::string timePrecision;
  bool hasOccurredAt = false;
  // The hub's view of this reader's own position for the same book, sent
  // alongside the suggestion. Used only to show the comparison when the reader
  // has no book open; the open book's real position always wins.
  bool hasLocal = false;
  double localPercentage = 0;
  uint32_t localCurrentPage = 0;
  uint32_t localTotalPages = 0;
};

// CrossPoint has no book-wide page numbering -- it paginates per chapter, and
// repaginates whenever the font changes -- so it reports position on this
// normalized page scale. A count of exactly this is therefore a percentage in
// disguise, not a page number any reader would recognise on screen.
inline constexpr uint32_t NORMALIZED_PAGE_SCALE = 10000;

// The position the other device left, as the resume core wants it.
ResumePosition remotePositionOf(const ProgressInboxItem& item);

// The hub's view of this reader's position for the same book. Only worth
// showing when the book is not open: the open book knows better.
ResumePosition hubLocalPositionOf(const ProgressInboxItem& item);

struct AnnotationInboxItem {
  uint64_t serverSequence = 0;
  uint64_t revision = 0;
  std::string directive;
  std::string eventType;
  std::string bookHash;
  std::string syncId;
  std::string text;
  std::string note;
  std::string xpointer;
  double percentage = 0;
  uint16_t spineIndex = 0;
  uint16_t chapterPageCount = 0;
  uint16_t chapterProgress = 0;
  bool deleted = false;
  bool conflict = false;
};

class DurableQueue {
 public:
  static constexpr size_t MAX_EVENT_BYTES = 6 * 1024;
  static constexpr size_t MAX_INBOX_EVENT_BYTES = 14 * 1024;
  static constexpr size_t MAX_BATCH_BYTES = 8 * 1024;
  static constexpr size_t MAX_BATCH_EVENTS = 2;
  static constexpr size_t MAX_QUEUE_SCAN = 200;

  static DurableQueue& instance();

  bool begin();
  std::optional<EventIdentity> reserveIdentity(std::string preferredId = {});
  bool enqueue(const EventIdentity& identity, const std::string& immutableEventJson);
  bool replaceUnattempted(const EventIdentity& identity, const std::string& eventJson);
  std::vector<PendingEvent> nextBatch(size_t maxEvents = MAX_BATCH_EVENTS, size_t maxBytes = MAX_BATCH_BYTES) const;
  bool markAttempted(const PendingEvent& event);
  bool retireAcknowledged(std::string_view eventId, uint64_t sequence);
  bool deferPulled(uint64_t serverSequence, const std::string& eventJson);
  std::optional<ProgressInboxItem> nextProgressDecision() const;
  // The newest position still waiting for an answer on one book. The reader
  // asks with this one so a suggestion for another book never interrupts.
  std::optional<ProgressInboxItem> nextSuggestionFor(std::string_view bookHash) const;
  std::optional<ProgressInboxItem> nextApplicableProgress(std::string_view bookHash) const;
  std::optional<AnnotationInboxItem> nextAnnotation(std::string_view bookHash, uint64_t afterSequence = 0) const;
  bool acceptProgress(const ProgressInboxItem& item);
  bool resolveProgress(uint64_t serverSequence);
  bool resolveProgressThrough(std::string_view bookHash, uint64_t serverSequence);
  bool resolveInbox(uint64_t serverSequence);
  size_t depth() const;
  size_t inboxDepth() const;
  // Drops every queued event, in and out, and rewinds the client sequence.
  // Called when the reader changes account: events raised for the old account
  // must never reach the new one. The clock anchor survives -- it is a time
  // reference, not account data.
  bool purgeAccountState();

  const Checkpoint& checkpoint() const { return state; }
  uint32_t getBootId() const { return runtimeBootId; }
  bool commitAppliedCursor(uint64_t cursor);
  bool setWallAnchor(uint64_t unixSeconds, uint32_t monotonicMs);

  // Write-then-rename with a backup, recovered on the next begin(). Shared so
  // every durable Toto file lands the same way on a card that can lose power
  // halfway through a write.
  static bool writeAtomic(const std::string& finalPath, const uint8_t* data, size_t size);

 private:
  DurableQueue() = default;

  Checkpoint state;
  uint32_t runtimeBootId = 0;
  bool initialized = false;

  bool loadCheckpoint();
  bool saveCheckpoint();
  static std::string eventFilename(uint64_t sequence, std::string_view id, bool attempted);
  static bool parseFilename(std::string_view filename, uint64_t& sequence, std::string& id, bool& attempted);
  static std::optional<ProgressInboxItem> readProgressInbox(std::string_view filename);
  static std::optional<AnnotationInboxItem> readAnnotationInbox(std::string_view filename);
  static bool removeInboxFile(uint64_t serverSequence);
};

}  // namespace toto

#define TOTO_QUEUE toto::DurableQueue::instance()
