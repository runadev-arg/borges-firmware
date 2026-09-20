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

  struct RecoveryOutcome {
    Result result = Result::INVALID_RESPONSE;
    bool found = false;
    int httpCode = 0;
  };

  static Outcome syncOnce();
  static RecoveryOutcome fetchProgress(const std::string& bookHash, bool otherDeviceOnly);
  static Result resolveSuggestion(const std::string& suggestionId, bool accept);
  static const char* resultName(Result result);

 private:
  static Outcome syncStep(bool allowUpload);
};

}  // namespace toto
