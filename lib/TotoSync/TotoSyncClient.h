#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace toto {

class SyncClient {
 public:
  enum class Result {
    OK,
    NOT_PAIRED,
    NOTHING_TO_DO,
    LOW_MEMORY,
    CLOCK_ERROR,
    NETWORK_ERROR,
    AUTH_ERROR,
    SERVER_ERROR,
    INVALID_RESPONSE,
    STORAGE_ERROR,
    // The hub had already closed this suggestion. Nothing left to deliver, so
    // the pending answer stops being retried instead of looping forever.
    ALREADY_RESOLVED,
  };

  struct Outcome {
    Result result = Result::INVALID_RESPONSE;
    size_t acknowledged = 0;
    size_t rejected = 0;
    size_t pulled = 0;
    size_t queueDepth = 0;
    uint64_t cursor = 0;
    int httpCode = 0;
    bool hasMore = false;
  };

  // `pullOnly` asks without sending, which is what a reconnection starts with:
  // draining first would make the hub take this reader's stale offline
  // position as the newest write and stop offering the other device's.
  static Outcome syncOnce(bool pullOnly = false);
  static Result resolveSuggestion(const std::string& suggestionId, bool accept);

  // Delivers the answers given while there was no signal. Returns how many are
  // still waiting afterwards, so the scheduler knows to come back.
  static size_t flushPendingResolutions();

  static const char* resultName(Result result);
};

}  // namespace toto
