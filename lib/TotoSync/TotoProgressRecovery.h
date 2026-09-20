#pragma once
#include <ArduinoJson.h>

#include <cmath>
#include <string>
#include <string_view>

#include "TotoPullValidation.h"

namespace toto {
struct RecoverySelection {
  bool valid = false;
  std::string eventJson;
};

inline bool validRecoveryBookHash(std::string_view hash) {
  if (hash.size() != 32) return false;
  for (char c : hash)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Explicit recovery reads snapshots, not the replication cursor. In particular,
// revision is NOT a device_events.server_sequence and must never advance it.
inline RecoverySelection selectRecoveryPosition(JsonObjectConst response, std::string_view bookHash,
                                                bool otherDeviceOnly) {
  if (!validRecoveryBookHash(bookHash) || (response["protocol_version"] | 0) != 2 ||
      !response["positions"].is<JsonArrayConst>())
    return {};
  JsonObjectConst selected;
  for (JsonVariantConst value : response["positions"].as<JsonArrayConst>()) {
    if (!value.is<JsonObjectConst>()) return {};
    const JsonObjectConst position = value.as<JsonObjectConst>();
    const JsonObjectConst identifier = position["book_identifier"].as<JsonObjectConst>();
    const auto revision = pullSequence(position["revision"]);
    const std::string_view recoveryId = position["recovery_id"] | "";
    const bool legacy = position["revision"].isNull() &&
                        std::string_view(position["source_protocol"] | "") == "legacy" &&
                        recoveryId.rfind("legacy:", 0) == 0 && recoveryId.size() > 7 && recoveryId.size() <= 256;
    if ((!legacy && (!revision || *revision == 0)) || !position["is_current_device"].is<bool>() ||
        std::string_view(identifier["kind"] | "") != "koreader_partial_md5" ||
        std::string_view(identifier["value"] | "") != bookHash || !position["percentage"].is<double>() ||
        !position["device_id"].is<const char*>())
      return {};
    const double percent = position["percentage"].as<double>();
    const std::string_view deviceId = position["device_id"].as<const char*>();
    if (!std::isfinite(percent) || percent < 0 || percent > 100 || deviceId.empty() || deviceId.size() > 128 ||
        (!position["xpointer"].isNull() && !position["xpointer"].is<const char*>()) ||
        (!position["device_name"].isNull() && !position["device_name"].is<const char*>()))
      return {};
    if (std::string_view(position["xpointer"] | "").size() > 4096) return {};
    if (otherDeviceOnly && position["is_current_device"].as<bool>()) continue;
    // The API orders received_at across legacy and v2. Their unrelated counters
    // cannot be compared; validate all rows, but keep the first eligible one.
    if (selected.isNull()) selected = position;
  }
  if (selected.isNull()) return {true, {}};
  JsonDocument event;
  event["event_type"] = "progress.changed";
  event["directive"] = "suggest_resume";
  event["book_identifier"]["kind"] = "koreader_partial_md5";
  event["book_identifier"]["value"] = std::string(bookHash);
  event["origin_device"]["id"] = selected["device_id"];
  event["origin_device"]["name"] = selected["device_name"] | selected["device_id"].as<const char*>();
  event["source_revision"] = selected["revision"];
  event["source_recovery_id"] = selected["recovery_id"];
  event["payload"]["percentage"] = selected["percentage"];
  event["payload"]["xpointer"] = selected["xpointer"] | "";
  event["payload"]["current_page"] = selected["current_page"] | 0;
  event["payload"]["total_pages"] = selected["total_pages"] | 0;
  RecoverySelection result{true, {}};
  serializeJson(event, result.eventJson);
  return result;
}
}  // namespace toto
