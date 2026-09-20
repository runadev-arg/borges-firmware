#include "TotoSyncCore.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <limits>

namespace toto {
namespace {

constexpr size_t CHECKSUM_OFFSET = CHECKPOINT_SIZE - sizeof(uint32_t);

void put16(std::array<uint8_t, CHECKPOINT_SIZE>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void put32(std::array<uint8_t, CHECKPOINT_SIZE>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8U));
  }
}

void put64(std::array<uint8_t, CHECKPOINT_SIZE>& bytes, size_t offset, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8U));
  }
}

uint16_t get16(const uint8_t* bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8U;
}

uint32_t get32(const uint8_t* bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
  }
  return value;
}

uint64_t get64(const uint8_t* bytes, size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8U);
  }
  return value;
}

bool validPrecision(uint8_t value) { return value <= static_cast<uint8_t>(TimePrecision::APPROXIMATE); }

uint64_t fnv1a64(std::string_view value, uint64_t basis) {
  constexpr uint64_t PRIME = 1099511628211ULL;
  uint64_t hash = basis;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= PRIME;
  }
  return hash;
}

char hexDigit(uint8_t value) { return "0123456789abcdef"[value & 0x0FU]; }

bool isHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

std::array<uint8_t, CHECKPOINT_SIZE> encodeCheckpoint(const Checkpoint& checkpoint) {
  std::array<uint8_t, CHECKPOINT_SIZE> bytes{};
  put32(bytes, 0, CHECKPOINT_MAGIC);
  put16(bytes, 4, CHECKPOINT_VERSION);
  bytes[6] = static_cast<uint8_t>(checkpoint.anchorPrecision);
  bytes[7] = 0;
  put32(bytes, 8, checkpoint.generation);
  put64(bytes, 12, checkpoint.nextSequence);
  put64(bytes, 20, checkpoint.appliedCursor);
  put64(bytes, 28, checkpoint.wallAnchorUnixSeconds);
  put32(bytes, 36, checkpoint.monotonicAnchorMs);
  put32(bytes, 40, checkpoint.bootId);
  put32(bytes, CHECKSUM_OFFSET, crc32(bytes.data(), CHECKSUM_OFFSET));
  return bytes;
}

std::optional<Checkpoint> decodeCheckpoint(const uint8_t* data, size_t size) {
  if (data == nullptr || size != CHECKPOINT_SIZE || get32(data, 0) != CHECKPOINT_MAGIC ||
      get16(data, 4) != CHECKPOINT_VERSION || !validPrecision(data[6]) ||
      get32(data, CHECKSUM_OFFSET) != crc32(data, CHECKSUM_OFFSET)) {
    return std::nullopt;
  }

  Checkpoint checkpoint;
  checkpoint.anchorPrecision = static_cast<TimePrecision>(data[6]);
  checkpoint.generation = get32(data, 8);
  checkpoint.nextSequence = get64(data, 12);
  checkpoint.appliedCursor = get64(data, 20);
  checkpoint.wallAnchorUnixSeconds = get64(data, 28);
  checkpoint.monotonicAnchorMs = get32(data, 36);
  checkpoint.bootId = get32(data, 40);
  if (checkpoint.nextSequence == 0) {
    return std::nullopt;
  }
  return checkpoint;
}

std::optional<Checkpoint> selectNewestCheckpoint(const std::vector<uint8_t>& first,
                                                 const std::vector<uint8_t>& second) {
  const auto a = decodeCheckpoint(first.data(), first.size());
  const auto b = decodeCheckpoint(second.data(), second.size());
  if (!a) return b;
  if (!b) return a;
  return static_cast<int32_t>(a->generation - b->generation) > 0 ? a : b;
}

uint32_t exponentialBackoffMs(uint8_t failureCount, uint32_t entropy) {
  const uint8_t exponent = std::min<uint8_t>(failureCount, 10);
  const uint64_t scaled = static_cast<uint64_t>(BACKOFF_BASE_MS) << exponent;
  const uint32_t bounded = static_cast<uint32_t>(std::min<uint64_t>(scaled, BACKOFF_MAX_MS));
  const uint32_t jitterRange = bounded / 4U;
  const uint32_t jitter = jitterRange == 0 ? 0 : entropy % (jitterRange + 1U);
  return bounded > BACKOFF_MAX_MS - jitter ? BACKOFF_MAX_MS : bounded + jitter;
}

ResolvedTime resolveWallTime(const Checkpoint& checkpoint, uint32_t currentBootId, uint32_t monotonicNowMs) {
  if (checkpoint.wallAnchorUnixSeconds == 0) {
    return {};
  }
  if (checkpoint.bootId == currentBootId) {
    const uint32_t elapsedMs = monotonicNowMs - checkpoint.monotonicAnchorMs;
    return {checkpoint.wallAnchorUnixSeconds + elapsedMs / 1000U, TimePrecision::ANCHORED};
  }
  return {checkpoint.wallAnchorUnixSeconds, TimePrecision::APPROXIMATE};
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  const int adjustedYear = year - (month <= 2 ? 1 : 0);
  const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
  const unsigned adjustedMonth = month > 2 ? month - 3U : month + 9U;
  const unsigned dayOfYear = (153U * adjustedMonth + 2U) / 5U + day - 1U;
  const unsigned dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

std::string formatRfc3339(uint64_t unixSeconds) {
  if (unixSeconds > static_cast<uint64_t>(std::numeric_limits<std::time_t>::max())) {
    return {};
  }
  const std::time_t value = static_cast<std::time_t>(unixSeconds);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &value) != 0) return {};
#else
  if (gmtime_r(&value, &utc) == nullptr) return {};
#endif
  std::array<char, 21> output{};
  if (std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) != 20) return {};
  return output.data();
}

std::optional<uint64_t> parseRfc3339Utc(std::string_view value) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value.back() != 'Z') {
    return std::nullopt;
  }
  if (value.size() != 20) {
    if (value[19] != '.' || value.size() < 22) return std::nullopt;
    for (size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return std::nullopt;
    }
  }
  auto decimal = [value](size_t offset, size_t count) -> std::optional<unsigned> {
    unsigned result = 0;
    for (size_t index = 0; index < count; ++index) {
      const char digit = value[offset + index];
      if (digit < '0' || digit > '9') return std::nullopt;
      result = result * 10U + static_cast<unsigned>(digit - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (!year || !month || !day || !hour || !minute || !second || *year < 1970 || *month < 1 || *month > 12 ||
      *hour > 23 || *minute > 59 || *second > 59) {
    return std::nullopt;
  }
  constexpr std::array<unsigned, 12> DAYS_PER_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leapYear = (*year % 4U == 0U && *year % 100U != 0U) || *year % 400U == 0U;
  const unsigned maxDay = *month == 2U && leapYear ? 29U : DAYS_PER_MONTH[*month - 1U];
  if (*day < 1U || *day > maxDay) return std::nullopt;

  const int64_t days = daysFromCivil(static_cast<int>(*year), *month, *day);
  if (days < 0) return std::nullopt;
  return static_cast<uint64_t>(days) * 86400U + *hour * 3600U + *minute * 60U + *second;
}

const char* timePrecisionName(TimePrecision precision) {
  switch (precision) {
    case TimePrecision::EXACT:
      return "exact";
    case TimePrecision::ANCHORED:
      return "anchored";
    case TimePrecision::APPROXIMATE:
      return "approximate";
    default:
      return "unknown";
  }
}

std::string formatUuidV4(const std::array<uint8_t, 16>& randomBytes) {
  auto bytes = randomBytes;
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3FU) | 0x80U);

  std::string result;
  result.reserve(36);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) result.push_back('-');
    result.push_back(hexDigit(bytes[i] >> 4U));
    result.push_back(hexDigit(bytes[i]));
  }
  return result;
}

std::string deterministicUuid(std::string_view seed) {
  const uint64_t first = fnv1a64(seed, 14695981039346656037ULL);
  const uint64_t second = fnv1a64(seed, 7809847782465536322ULL);
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>(first >> (i * 8U));
    bytes[i + 8] = static_cast<uint8_t>(second >> (i * 8U));
  }
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0FU) | 0x50U);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  return formatUuidV4(bytes).replace(14, 1, "5");
}

bool isUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if (separator ? value[i] != '-' : !isHex(value[i])) return false;
  }
  return true;
}

bool acknowledgmentMatches(std::string_view eventId, uint64_t sequence, std::string_view acknowledgedId,
                           std::string_view acknowledgedSequence) {
  if (eventId != acknowledgedId || acknowledgedSequence.empty()) return false;
  uint64_t parsed = 0;
  for (char digit : acknowledgedSequence) {
    if (digit < '0' || digit > '9') return false;
    const uint8_t value = static_cast<uint8_t>(digit - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - value) / 10U) return false;
    parsed = parsed * 10U + value;
  }
  return parsed == sequence;
}

ProgressDirective parseProgressDirective(std::string_view value) {
  if (value == "own") return ProgressDirective::OWN;
  if (value == "keep_local") return ProgressDirective::KEEP_LOCAL;
  if (value == "apply_initial") return ProgressDirective::APPLY_INITIAL;
  if (value == "equivalent") return ProgressDirective::EQUIVALENT;
  if (value == "merge_forward") return ProgressDirective::MERGE_FORWARD;
  if (value == "suggest_resume") return ProgressDirective::SUGGEST_RESUME;
  if (value == "suggest_jump") return ProgressDirective::SUGGEST_JUMP;
  return ProgressDirective::UNKNOWN;
}

bool shouldAutoApply(ProgressDirective directive) {
  // The server can recommend a location, but only the reader may authorize a jump.
  (void)directive;
  return false;
}

bool mayUploadAfterPull(bool succeeded, bool hasMore) { return succeeded && !hasMore; }

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return deadlineMs != 0 && static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

bool readerSyncWindowReady(uint32_t nowMs, uint32_t nextAttemptMs, uint32_t lastSuccessMs, bool pageSettled,
                           bool buildActive) {
  if (!pageSettled || buildActive || !deadlineReached(nowMs, nextAttemptMs)) return false;
  return lastSuccessMs == 0 || static_cast<uint32_t>(nowMs - lastSuccessMs) >= READER_SYNC_COOLDOWN_MS;
}

std::string boundedUtf8(std::string_view value, size_t maxBytes) {
  if (value.size() <= maxBytes) return std::string(value);
  size_t size = maxBytes;
  while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xC0U) == 0x80U) --size;
  return std::string(value.substr(0, size));
}

std::string annotationFingerprint(std::string_view syncId, std::string_view text, std::string_view note,
                                  std::string_view xpointer, uint32_t basisPoints) {
  std::string seed;
  seed.reserve(syncId.size() + text.size() + note.size() + xpointer.size() + 32);
  seed.append(syncId);
  seed.push_back('|');
  seed.append(text);
  seed.push_back('|');
  seed.append(note);
  seed.push_back('|');
  seed.append(xpointer);
  seed.push_back('|');
  seed.append(std::to_string(basisPoints));
  return deterministicUuid(seed);
}

std::string legacyBookmarkSyncId(std::string_view bookHash, std::string_view xpointer, uint32_t basisPoints) {
  std::string seed = "crosspoint-bookmark:";
  seed.append(bookHash);
  seed.push_back('|');
  seed.append(xpointer);
  seed.push_back('|');
  seed.append(std::to_string(basisPoints));
  return deterministicUuid(seed);
}

}  // namespace toto
