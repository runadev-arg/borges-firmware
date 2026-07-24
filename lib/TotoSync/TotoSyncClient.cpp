#include "TotoSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <charconv>
#include <string>
#include <vector>

#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoTrust.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace {

constexpr uint32_t MIN_FREE_FOR_TLS = 50000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;
constexpr size_t MAX_RESPONSE_BYTES = 14 * 1024;
constexpr uint8_t PULL_LIMIT = 2;

bool insufficientHeap() { return ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS; }

std::optional<uint64_t> decimalSequence(const char* value) {
  if (value == nullptr || *value == '\0') return std::nullopt;
  uint64_t parsed = 0;
  const char* end = value;
  while (*end != '\0') ++end;
  const auto result = std::from_chars(value, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return parsed;
}

std::string clientTelemetry(size_t queueDepth) {
  JsonDocument client;
  client["protocol_version"] = 2;
  client["client_version"] = std::string("crosspoint-toto/") + CROSSPOINT_VERSION;
  client["queue_depth"] = queueDepth;
  JsonObject capabilities = client["capabilities"].to<JsonObject>();
  capabilities["atomic_outbox"] = true;
  capabilities["dual_checkpoint"] = true;
  capabilities["bounded_pull"] = PULL_LIMIT;
  capabilities["annotation_revisions"] = true;
  capabilities["annotation_conflicts"] = true;
  capabilities["annotation_tombstones"] = true;
  capabilities["max_annotation_bytes"] = DurableQueue::MAX_EVENT_BYTES;
  std::string value;
  serializeJson(client, value);
  return value;
}

std::string exchangeBody(uint64_t cursor, const std::vector<PendingEvent>& batch, size_t queueDepth) {
  std::string body = "{\"protocol_version\":2,\"cursor\":\"" + std::to_string(cursor) +
                     "\",\"pull_limit\":" + std::to_string(PULL_LIMIT) + ",\"events\":[";
  for (size_t index = 0; index < batch.size(); ++index) {
    if (index > 0) body.push_back(',');
    body += batch[index].body;
  }
  body += "],\"client\":";
  body += clientTelemetry(queueDepth);
  body.push_back('}');
  return body;
}

std::string pullBody(uint64_t cursor, size_t queueDepth) {
  return "{\"protocol_version\":2,\"cursor\":\"" + std::to_string(cursor) +
         "\",\"limit\":" + std::to_string(PULL_LIMIT) + ",\"client\":" + clientTelemetry(queueDepth) + "}";
}

int post(const std::string& path, const std::string& body, std::string& response) {
  if (TOTO_CREDENTIALS.getBaseUrl().rfind("https://", 0) != 0) return -3;
  freeink::SecureHttpClient http;
  http.setCACert(rootCertificate());
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setUserAgent(std::string("CrossPoint-Toto/") + CROSSPOINT_VERSION);
  if (!http.begin(TOTO_CREDENTIALS.getBaseUrl() + path)) return -1;
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + TOTO_CREDENTIALS.getToken());

  response.clear();
  response.reserve(4096);
  const int code = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                    [&response](const uint8_t* data, size_t size) {
                                      if (response.size() + size > MAX_RESPONSE_BYTES) return false;
                                      response.append(reinterpret_cast<const char*>(data), size);
                                      return true;
                                    });
  const bool complete = http.responseComplete() && !http.callbackAborted();
  http.end();
  return complete ? code : -2;
}

bool shouldDefer(JsonObjectConst event) {
  const std::string type = event["event_type"] | "";
  if (type.rfind("annotation.", 0) == 0 || type.rfind("bookmark.", 0) == 0) return true;
  const std::string directive = event["directive"] | "";
  if (directive == "own" || directive == "keep_local" || directive == "equivalent") return false;
  return type == "progress.changed";
}

}  // namespace

SyncClient::Outcome SyncClient::syncOnce() {
  Outcome outcome;
  if (!TOTO_CREDENTIALS.paired()) {
    outcome.result = Result::NOT_PAIRED;
    return outcome;
  }
  if (!TOTO_QUEUE.begin()) {
    outcome.result = Result::STORAGE_ERROR;
    return outcome;
  }
  outcome.queueDepth = TOTO_QUEUE.depth();
  outcome.cursor = TOTO_QUEUE.checkpoint().appliedCursor;
  if (insufficientHeap()) {
    outcome.result = Result::LOW_MEMORY;
    return outcome;
  }
  if (!ensureTrustedClock()) {
    outcome.result = Result::CLOCK_ERROR;
    return outcome;
  }

  const std::vector<PendingEvent> batch = TOTO_QUEUE.nextBatch();
  const bool exchanging = !batch.empty();
  if (exchanging) {
    for (const PendingEvent& event : batch) {
      if (!TOTO_QUEUE.markAttempted(event)) {
        outcome.result = Result::STORAGE_ERROR;
        return outcome;
      }
    }
  }

  const std::string body = exchanging ? exchangeBody(outcome.cursor, batch, outcome.queueDepth)
                                      : pullBody(outcome.cursor, outcome.queueDepth);
  std::string response;
  const int code = post(exchanging ? "/api/sync/v2/exchange" : "/api/sync/v2/pull", body, response);
  outcome.httpCode = code > 0 ? code : 0;
  if (code <= 0) {
    outcome.result = Result::NETWORK_ERROR;
    return outcome;
  }
  if (code == 401 || code == 403) {
    outcome.result = Result::AUTH_ERROR;
    return outcome;
  }
  if (code != 200) {
    outcome.result = Result::SERVER_ERROR;
    return outcome;
  }

  JsonDocument document;
  if (deserializeJson(document, response) || (document["protocol_version"] | 0) != 2) {
    outcome.result = Result::INVALID_RESPONSE;
    return outcome;
  }

  if (exchanging) {
    for (JsonObjectConst acknowledgement : document["acknowledgements"].as<JsonArrayConst>()) {
      const char* id = acknowledgement["client_event_id"] | "";
      const char* sequenceText = acknowledgement["client_sequence"] | "";
      const std::string status = acknowledgement["status"] | "";
      for (const PendingEvent& event : batch) {
        if (!acknowledgmentMatches(event.id, event.sequence, id, sequenceText)) continue;
        if (status == "accepted" || status == "rejected") {
          if (!TOTO_QUEUE.retireAcknowledged(event.id, event.sequence)) {
            outcome.result = Result::STORAGE_ERROR;
            return outcome;
          }
          if (status == "accepted") {
            ++outcome.acknowledged;
          } else {
            ++outcome.rejected;
            TOTO_CREDENTIALS.setLastError(acknowledgement["rejection_code"] | "event_rejected");
          }
        }
      }
    }
  }

  const JsonObjectConst pull = exchanging ? document["pull"].as<JsonObjectConst>() : document.as<JsonObjectConst>();
  uint64_t lastSeenSequence = outcome.cursor;
  for (JsonObjectConst event : pull["events"].as<JsonArrayConst>()) {
    const auto serverSequence = decimalSequence(event["server_sequence"] | "");
    if (!serverSequence || *serverSequence <= lastSeenSequence) {
      outcome.result = Result::INVALID_RESPONSE;
      return outcome;
    }
    if (shouldDefer(event)) {
      std::string serialized;
      serializeJson(event, serialized);
      if (!TOTO_QUEUE.deferPulled(*serverSequence, serialized)) {
        outcome.result = Result::STORAGE_ERROR;
        return outcome;
      }
    }
    lastSeenSequence = *serverSequence;
    ++outcome.pulled;
  }

  const auto responseCursor = decimalSequence(pull["cursor"] | "");
  if (!responseCursor || *responseCursor < lastSeenSequence || *responseCursor < outcome.cursor ||
      !TOTO_QUEUE.commitAppliedCursor(*responseCursor)) {
    outcome.result = Result::INVALID_RESPONSE;
    return outcome;
  }
  outcome.cursor = *responseCursor;
  outcome.hasMore = pull["has_more"] | false;

  if (const auto serverTime = parseRfc3339Utc(document["server_time"] | ""); serverTime.has_value()) {
    TOTO_QUEUE.setWallAnchor(*serverTime, millis());
  }
  outcome.queueDepth = TOTO_QUEUE.depth();
  if (outcome.rejected == 0) TOTO_CREDENTIALS.setLastError({});
  TOTO_CREDENTIALS.saveToFile();
  outcome.result = Result::OK;
  return outcome;
}

SyncClient::Result SyncClient::resolveSuggestion(const std::string& suggestionId, bool accept) {
  if (!TOTO_CREDENTIALS.paired() || !isUuid(suggestionId)) return Result::INVALID_RESPONSE;
  if (insufficientHeap()) return Result::LOW_MEMORY;
  if (!ensureTrustedClock()) return Result::CLOCK_ERROR;
  std::string response;
  const int code = post("/api/sync/v2/suggestions/" + suggestionId + (accept ? "/accept" : "/dismiss"), "{}", response);
  if (code <= 0) return Result::NETWORK_ERROR;
  if (code == 401 || code == 403) return Result::AUTH_ERROR;
  if (code != 200) return Result::SERVER_ERROR;
  JsonDocument document;
  if (deserializeJson(document, response) || (document["protocol_version"] | 0) != 2) {
    return Result::INVALID_RESPONSE;
  }
  return Result::OK;
}

const char* SyncClient::resultName(Result result) {
  switch (result) {
    case Result::OK:
      return "ok";
    case Result::NOT_PAIRED:
      return "not_paired";
    case Result::NOTHING_TO_DO:
      return "nothing_to_do";
    case Result::LOW_MEMORY:
      return "low_memory";
    case Result::CLOCK_ERROR:
      return "clock_error";
    case Result::NETWORK_ERROR:
      return "network_error";
    case Result::AUTH_ERROR:
      return "auth_error";
    case Result::SERVER_ERROR:
      return "server_error";
    case Result::INVALID_RESPONSE:
      return "invalid_response";
    case Result::STORAGE_ERROR:
      return "storage_error";
  }
  return "unknown";
}

}  // namespace toto
