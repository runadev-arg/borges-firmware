#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
};

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
  std::optional<ProgressInboxItem> nextApplicableProgress(std::string_view bookHash) const;
  std::optional<AnnotationInboxItem> nextAnnotation(std::string_view bookHash, uint64_t afterSequence = 0) const;
  bool acceptProgress(const ProgressInboxItem& item);
  bool resolveProgress(uint64_t serverSequence);
  bool resolveProgressThrough(std::string_view bookHash, uint64_t serverSequence);
  bool resolveInbox(uint64_t serverSequence);
  size_t depth() const;
  size_t inboxDepth() const;

  const Checkpoint& checkpoint() const { return state; }
  uint32_t getBootId() const { return runtimeBootId; }
  bool commitAppliedCursor(uint64_t cursor);
  bool setWallAnchor(uint64_t unixSeconds, uint32_t monotonicMs);

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
  static bool writeAtomic(const std::string& finalPath, const uint8_t* data, size_t size);
};

}  // namespace toto

#define TOTO_QUEUE toto::DurableQueue::instance()
