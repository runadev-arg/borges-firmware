#pragma once
#include <ArduinoJson.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace borges {
struct ValidatedPull {
  uint64_t cursor;
  uint64_t highWatermark;
  bool hasMore;
};

inline std::optional<uint64_t> pullSequence(JsonVariantConst value) {
  if (!value.is<const char*>()) return std::nullopt;
  const std::string_view text = value.as<const char*>();
  if (text.empty()) return std::nullopt;
  uint64_t sequence = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), sequence);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
  return sequence;
}

inline std::optional<ValidatedPull> validatePull(JsonObjectConst pull, uint64_t previousCursor) {
  if (pull.isNull() || !pull["events"].is<JsonArrayConst>() || !pull["has_more"].is<bool>()) return std::nullopt;
  const auto cursor = pullSequence(pull["cursor"]);
  const auto watermark = pullSequence(pull["high_watermark"]);
  if (!cursor || !watermark || *cursor < previousCursor || *watermark < *cursor) return std::nullopt;
  const bool hasMore = pull["has_more"].as<bool>();
  if (hasMore ? (*cursor == *watermark || *cursor == previousCursor) : (*cursor != *watermark)) return std::nullopt;
  uint64_t last = previousCursor;
  for (JsonVariantConst value : pull["events"].as<JsonArrayConst>()) {
    if (!value.is<JsonObjectConst>() || !value["event_type"].is<const char*>()) return std::nullopt;
    const auto sequence = pullSequence(value["server_sequence"]);
    if (!sequence || *sequence <= last || *sequence > *cursor) return std::nullopt;
    last = *sequence;
  }
  return ValidatedPull{*cursor, *watermark, hasMore};
}
}  // namespace borges
