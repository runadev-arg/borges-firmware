#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "TotoDurableQueue.h"

namespace toto {

class ReadingEvents {
 public:
  static ReadingEvents& instance();

  bool beginBook(const std::string& epubPath, std::string title, std::string author);
  bool recordPosition(float percentage, const std::string& xpointer, bool explicitJump = false);
  bool endBook();
  bool retryPendingProgress();
  bool hasPendingProgress() const { return progressPending; }
  bool active() const { return !bookHash.empty(); }
  const std::string& getBookHash() const { return bookHash; }
  uint32_t getBootId() const { return bootId; }

 private:
  static constexpr size_t PAGE_ROWS_PER_EVENT = 20;
  static constexpr uint32_t NORMALIZED_TOTAL_PAGES = 10000;
  static constexpr uint32_t MIN_SESSION_SECONDS = 60;
  static constexpr uint32_t MAX_SESSION_SECONDS = 4U * 60U * 60U;

  struct PageRow {
    uint32_t page = 0;
    uint32_t durationSeconds = 0;
    uint64_t startedUnixSeconds = 0;
  };

  ReadingEvents() = default;

  std::string bookHash;
  std::string title;
  std::string author;
  std::string sessionId;
  uint32_t bootId = 0;
  uint32_t sessionStartedMs = 0;
  uint64_t sessionStartedUnixSeconds = 0;
  uint32_t pageStartedMs = 0;
  uint32_t currentPage = 0;
  uint32_t pagesRead = 0;
  bool havePage = false;
  std::array<char, 512> lastObservedXpointer{};
  size_t lastObservedXpointerSize = 0;
  bool progressPending = false;
  float pendingPercentage = 0;
  bool pendingExplicitJump = false;
  ResolvedTime pendingProgressTime;
  EventIdentity progressIdentity;
  EventIdentity pageStatsIdentity;
  std::array<PageRow, PAGE_ROWS_PER_EVENT> pageRows{};
  size_t pageRowCount = 0;
  uint32_t statsBucket = 0;

  ResolvedTime now() const;
  bool enqueueStarted();
  bool persistProgress(float percentage, const std::string& xpointer, bool explicitJump, ResolvedTime occurredAt);
  void finishCurrentPage();
  bool persistPageStats();
  void persistEnded(uint32_t totalDurationSeconds);
  std::string serializeCommon(const EventIdentity& identity, const char* eventType, const char* aggregateType,
                              const std::string& aggregateId, const JsonDocument& payload, uint64_t occurredAt,
                              TimePrecision precision) const;
};

}  // namespace toto

#define TOTO_READING_EVENTS toto::ReadingEvents::instance()
