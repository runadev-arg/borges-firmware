#include "BorgesReadingEvents.h"

#include <ArduinoJson.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <utility>

#include "BorgesCredentialStore.h"
#include "BorgesSyncScheduler.h"

namespace borges {
namespace {

std::string truncate(std::string value, size_t limit) {
  if (value.size() > limit) value.resize(limit);
  return value;
}

}  // namespace

ReadingEvents& ReadingEvents::instance() {
  static ReadingEvents events;
  return events;
}

bool ReadingEvents::beginBook(const std::string& epubPath, std::string bookTitle, std::string bookAuthor) {
  // Never replace the only in-memory retry of a failed SD write with another book.
  if (active() && !endBook()) return false;
  if (!BORGES_CREDENTIALS.paired() || !BORGES_QUEUE.begin()) return false;
  bookHash = KOReaderDocumentId::calculate(epubPath);
  if (bookHash.size() != 32) {
    bookHash.clear();
    return false;
  }

  title = truncate(std::move(bookTitle), 500);
  author = truncate(std::move(bookAuthor), 500);
  bootId = BORGES_QUEUE.getBootId();
  sessionStartedMs = millis();
  pageStartedMs = sessionStartedMs;
  const auto wall = now();
  sessionStartedUnixSeconds = wall.unixSeconds;
  pagesRead = 0;
  havePage = false;
  lastObservedXpointerSize = 0;
  progressPending = false;
  pageRowCount = 0;
  statsBucket = 0;
  progressIdentity = {};
  pageStatsIdentity = {};

  std::array<uint8_t, 16> random{};
  esp_fill_random(random.data(), random.size());
  sessionId = formatUuidV4(random);
  return enqueueStarted();
}

bool ReadingEvents::recordPosition(float percentage, const std::string& xpointer, bool explicitJump) {
  if (!active()) return true;
  percentage = std::clamp(percentage, 0.0f, 1.0f);
  const uint32_t page = static_cast<uint32_t>(std::lround(percentage * static_cast<float>(NORMALIZED_TOTAL_PAGES)));

  // A menu rerender or pagination-cache update is not a fresh reading movement.
  const bool sameLocator = xpointer.size() <= lastObservedXpointer.size() &&
                           xpointer.size() == lastObservedXpointerSize &&
                           std::equal(xpointer.begin(), xpointer.end(), lastObservedXpointer.begin());
  const bool samePosition = havePage && page == currentPage && sameLocator && !explicitJump;
  if (samePosition && !progressPending) return true;
  if (!samePosition) pendingProgressTime = now();
  lastObservedXpointerSize = std::min(xpointer.size(), lastObservedXpointer.size());
  std::copy_n(xpointer.data(), lastObservedXpointerSize, lastObservedXpointer.begin());

  if (!havePage) {
    currentPage = page;
    pageStartedMs = millis();
    havePage = true;
    // Opening an old local page is not a new reading movement. In particular,
    // reconnecting/reopening must not re-date it after another device's progress.
    if (!explicitJump) return true;
  } else if (page != currentPage) {
    finishCurrentPage();
    currentPage = page;
    pageStartedMs = millis();
    ++pagesRead;
  }
  pendingPercentage = percentage;
  if (!samePosition || !progressPending) pendingExplicitJump = explicitJump;
  progressPending = !persistProgress(percentage, xpointer, pendingExplicitJump, pendingProgressTime);
  if (progressPending) LOG_ERR("BORGES", "Progress is local but not queued; SD write will be retried");
  return !progressPending;
}

bool ReadingEvents::retryPendingProgress() {
  if (!progressPending) return true;
  const std::string locator(lastObservedXpointer.data(), lastObservedXpointerSize);
  progressPending = !persistProgress(pendingPercentage, locator, pendingExplicitJump, pendingProgressTime);
  return !progressPending;
}

bool ReadingEvents::endBook() {
  if (!active()) return true;
  if (!retryPendingProgress()) return false;
  finishCurrentPage();
  persistPageStats();
  const uint32_t durationSeconds = (millis() - sessionStartedMs) / 1000U;
  if (durationSeconds >= MIN_SESSION_SECONDS) persistEnded(durationSeconds);

  bookHash.clear();
  title.clear();
  author.clear();
  sessionId.clear();
  progressIdentity = {};
  pageStatsIdentity = {};
  havePage = false;
  pageRowCount = 0;
  return true;
}

ResolvedTime ReadingEvents::now() const {
  constexpr std::time_t MIN_TRUSTED_TIME = 1767225600;  // 2026-01-01
  const std::time_t systemTime = std::time(nullptr);
  if (systemTime >= MIN_TRUSTED_TIME) {
    return {
        .unixSeconds = static_cast<uint64_t>(systemTime),
        .precision = TimePrecision::EXACT,
    };
  }
  return resolveWallTime(BORGES_QUEUE.checkpoint(), bootId, millis());
}

bool ReadingEvents::enqueueStarted() {
  const auto identity = BORGES_QUEUE.reserveIdentity(deterministicUuid(sessionId + ":session.started"));
  if (!identity) return false;
  JsonDocument payload;
  payload["session_id"] = sessionId;
  payload["source"] = "crosspoint-x4";
  const auto wall = now();
  const std::string event = serializeCommon(*identity, "session.started", "reading_session", sessionId, payload,
                                            wall.unixSeconds, wall.precision);
  const bool queued = !event.empty() && BORGES_QUEUE.enqueue(*identity, event);
  if (queued) BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
  return queued;
}

bool ReadingEvents::persistProgress(float percentage, const std::string& xpointer, bool explicitJump,
                                    ResolvedTime wall) {
  if (progressIdentity.id.empty()) {
    const auto identity =
        BORGES_QUEUE.reserveIdentity(deterministicUuid(sessionId + ":progress:" + std::to_string(statsBucket)));
    if (!identity) return false;
    progressIdentity = *identity;
  }

  JsonDocument payload;
  payload["percentage"] = static_cast<double>(percentage * 100.0f);
  payload["current_page"] = currentPage;
  payload["total_pages"] = NORMALIZED_TOTAL_PAGES;
  if (!xpointer.empty() && xpointer.size() <= 512) payload["xpointer"] = xpointer;
  payload["explicit_jump"] = explicitJump;
  const std::string event =
      serializeCommon(progressIdentity, "progress.changed", "reading_progress", "koreader_partial_md5:" + bookHash,
                      payload, wall.unixSeconds, wall.precision);
  if (event.empty()) return false;
  if (BORGES_QUEUE.replaceUnattempted(progressIdentity, event)) {
    BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
    return true;
  }

  const auto replacement = BORGES_QUEUE.reserveIdentity();
  if (!replacement) return false;
  progressIdentity = *replacement;
  const bool queued = BORGES_QUEUE.enqueue(
      progressIdentity, serializeCommon(progressIdentity, "progress.changed", "reading_progress",
                                        "koreader_partial_md5:" + bookHash, payload, wall.unixSeconds, wall.precision));
  if (queued) BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
  return queued;
}

void ReadingEvents::finishCurrentPage() {
  if (!havePage) return;
  const uint32_t durationSeconds = (millis() - pageStartedMs) / 1000U;
  if (durationSeconds == 0) return;
  if (pageRowCount >= PAGE_ROWS_PER_EVENT) {
    persistPageStats();
    pageRowCount = 0;
    pageStatsIdentity = {};
    ++statsBucket;
  }
  const auto wall = now();
  pageRows[pageRowCount++] = {
      .page = currentPage,
      .durationSeconds = durationSeconds,
      .startedUnixSeconds = wall.unixSeconds >= durationSeconds ? wall.unixSeconds - durationSeconds : 0,
  };
  persistPageStats();
}

bool ReadingEvents::persistPageStats() {
  if (pageRowCount == 0) return true;
  if (pageStatsIdentity.id.empty()) {
    const auto identity =
        BORGES_QUEUE.reserveIdentity(deterministicUuid(sessionId + ":page_stats:" + std::to_string(statsBucket)));
    if (!identity) return false;
    pageStatsIdentity = *identity;
  }

  JsonDocument payload;
  JsonArray rows = payload["rows"].to<JsonArray>();
  for (size_t index = 0; index < pageRowCount; ++index) {
    const PageRow& page = pageRows[index];
    JsonObject row = rows.add<JsonObject>();
    row["book_md5"] = bookHash;
    row["page"] = page.page;
    row["start_time"] = formatRfc3339(page.startedUnixSeconds);
    row["duration"] = page.durationSeconds;
    row["total_pages"] = NORMALIZED_TOTAL_PAGES;
  }
  JsonObject book = payload["books"].to<JsonArray>().add<JsonObject>();
  book["md5"] = bookHash;
  book["title"] = title;
  book["authors"] = author;
  book["pages"] = NORMALIZED_TOTAL_PAGES;

  const auto wall = now();
  const std::string event =
      serializeCommon(pageStatsIdentity, "page_stat.recorded", "reading_statistics",
                      sessionId + ":" + std::to_string(statsBucket), payload, wall.unixSeconds, wall.precision);
  if (event.empty()) return false;
  if (BORGES_QUEUE.replaceUnattempted(pageStatsIdentity, event)) {
    BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
    return true;
  }

  ++statsBucket;
  const auto replacement =
      BORGES_QUEUE.reserveIdentity(deterministicUuid(sessionId + ":page_stats:" + std::to_string(statsBucket)));
  if (!replacement) return false;
  pageStatsIdentity = *replacement;
  const bool queued = BORGES_QUEUE.enqueue(
      pageStatsIdentity,
      serializeCommon(pageStatsIdentity, "page_stat.recorded", "reading_statistics",
                      sessionId + ":" + std::to_string(statsBucket), payload, wall.unixSeconds, wall.precision));
  if (queued) BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
  return queued;
}

void ReadingEvents::persistEnded(uint32_t totalDurationSeconds) {
  uint32_t remaining = totalDurationSeconds;
  uint32_t offset = 0;
  uint32_t chunkIndex = 0;
  while (remaining >= MIN_SESSION_SECONDS) {
    uint32_t duration = std::min(remaining, MAX_SESSION_SECONDS);
    if (remaining > MAX_SESSION_SECONDS && remaining - duration < MIN_SESSION_SECONDS) {
      duration = remaining - MIN_SESSION_SECONDS;
    }
    const auto identity =
        BORGES_QUEUE.reserveIdentity(deterministicUuid(sessionId + ":session.ended:" + std::to_string(chunkIndex)));
    if (!identity) return;

    const uint64_t startedAt = sessionStartedUnixSeconds + offset;
    JsonDocument payload;
    payload["started_at"] = formatRfc3339(startedAt);
    payload["ended_at"] = formatRfc3339(startedAt + duration);
    payload["duration_seconds"] = duration;
    payload["pages_read"] = chunkIndex == 0 ? pagesRead : 0;
    payload["start_page"] = 0;
    payload["end_page"] = currentPage;
    payload["timezone"] = "UTC";
    payload["explicit_jump"] = false;
    const auto wall = now();
    const std::string event =
        serializeCommon(*identity, "session.ended", "reading_session", sessionId + ":" + std::to_string(chunkIndex),
                        payload, wall.unixSeconds, wall.precision);
    if (event.empty() || !BORGES_QUEUE.enqueue(*identity, event)) return;
    BORGES_SYNC_SCHEDULER.notifyLifecycleCommit();
    remaining -= duration;
    offset += duration;
    ++chunkIndex;
  }
}

std::string ReadingEvents::serializeCommon(const EventIdentity& identity, const char* eventType,
                                           const char* aggregateType, const std::string& aggregateId,
                                           const JsonDocument& payload, uint64_t occurredAt,
                                           TimePrecision precision) const {
  JsonDocument event;
  event["client_event_id"] = identity.id;
  event["client_sequence"] = std::to_string(identity.sequence);
  event["event_version"] = 1;
  event["event_type"] = eventType;
  event["aggregate_type"] = aggregateType;
  event["aggregate_id"] = aggregateId;
  JsonObject identifier = event["book_identifier"].to<JsonObject>();
  identifier["kind"] = "koreader_partial_md5";
  identifier["value"] = bookHash;
  event["occurred_at"] = formatRfc3339(occurredAt);
  event["time_precision"] = timePrecisionName(precision);
  event["monotonic_ms"] = std::to_string(millis());
  event["payload"] = payload;
  std::string serialized;
  serializeJson(event, serialized);
  if (event.overflowed() || serialized.size() > DurableQueue::MAX_EVENT_BYTES) return {};
  return serialized;
}

}  // namespace borges
