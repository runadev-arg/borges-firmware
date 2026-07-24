#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace toto {

constexpr uint32_t CHECKPOINT_MAGIC = 0x4F544F54U;  // "TOTO" in little endian
constexpr uint16_t CHECKPOINT_VERSION = 1;
constexpr size_t CHECKPOINT_SIZE = 48;
constexpr uint32_t BACKOFF_BASE_MS = 15000;
constexpr uint32_t BACKOFF_MAX_MS = 6U * 60U * 60U * 1000U;

enum class TimePrecision : uint8_t {
  UNKNOWN = 0,
  EXACT = 1,
  ANCHORED = 2,
  APPROXIMATE = 3,
};

struct Checkpoint {
  uint32_t generation = 0;
  uint64_t nextSequence = 1;
  uint64_t appliedCursor = 0;
  uint64_t wallAnchorUnixSeconds = 0;
  uint32_t monotonicAnchorMs = 0;
  uint32_t bootId = 0;
  TimePrecision anchorPrecision = TimePrecision::UNKNOWN;

  bool operator==(const Checkpoint&) const = default;
};

struct ResolvedTime {
  uint64_t unixSeconds = 0;
  TimePrecision precision = TimePrecision::UNKNOWN;
};

enum class ProgressDirective : uint8_t {
  UNKNOWN = 0,
  OWN,
  KEEP_LOCAL,
  APPLY_INITIAL,
  EQUIVALENT,
  MERGE_FORWARD,
  SUGGEST_RESUME,
  SUGGEST_JUMP,
};

uint32_t crc32(const uint8_t* data, size_t size);
std::array<uint8_t, CHECKPOINT_SIZE> encodeCheckpoint(const Checkpoint& checkpoint);
std::optional<Checkpoint> decodeCheckpoint(const uint8_t* data, size_t size);
std::optional<Checkpoint> selectNewestCheckpoint(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second);

uint32_t exponentialBackoffMs(uint8_t failureCount, uint32_t entropy);
ResolvedTime resolveWallTime(const Checkpoint& checkpoint, uint32_t currentBootId, uint32_t monotonicNowMs);
std::string formatRfc3339(uint64_t unixSeconds);
std::optional<uint64_t> parseRfc3339Utc(std::string_view value);
const char* timePrecisionName(TimePrecision precision);

std::string formatUuidV4(const std::array<uint8_t, 16>& randomBytes);
std::string deterministicUuid(std::string_view seed);
bool isUuid(std::string_view value);

bool acknowledgmentMatches(std::string_view eventId, uint64_t sequence, std::string_view acknowledgedId,
                           std::string_view acknowledgedSequence);
ProgressDirective parseProgressDirective(std::string_view value);
bool shouldAutoApply(ProgressDirective directive);

}  // namespace toto
