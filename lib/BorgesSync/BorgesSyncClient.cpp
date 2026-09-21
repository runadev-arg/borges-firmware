#include "BorgesSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <charconv>
#include <string>
#include <vector>

#include "BorgesCredentialStore.h"
#include "BorgesDurableQueue.h"
#include "BorgesNetBoot.h"
#include "BorgesProgressRecovery.h"
#include "BorgesPullValidation.h"
#include "BorgesTrust.h"

#ifndef BORGES_VERSION
#define BORGES_VERSION "development"
#endif

namespace borges {
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
  client["client_version"] = std::string("crosspoint-borges/") + BORGES_VERSION;
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

int request(const char* method, const std::string& path, const std::string& body, std::string& response) {
  // Same bootstrap as pairing: without it the sync stays dead on exactly the
  // networks the pairing fix was written for. No candidate loop here -- the
  // scheduler already retries.
  netboot::WifiFullPowerScope wifiFullPower;
  const std::string baseUrl = BORGES_CREDENTIALS.getBaseUrl();
  if (baseUrl.rfind("https://", 0) != 0) return -3;
  const std::string host = netboot::hostFromBaseUrl(baseUrl);
  if (host.empty()) return -3;
  const netboot::Candidates candidates = netboot::resolveServer(host.c_str());  // memoized
  if (candidates.count == 0) return -1;

  freeink::SecureHttpClient http;
  http.setCACert(rootCertificate());
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setUserAgent(std::string("Borges-Borges/") + BORGES_VERSION);
  if (!http.begin(baseUrl + path)) return -1;
  http.setServerAddress(candidates.ip[0]);  // begin() FIRST, pin AFTER
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + BORGES_CREDENTIALS.getToken());

  response.clear();
  response.reserve(4096);
  const int code = http.sendRequest(method, reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                    [&response](const uint8_t* data, size_t size) {
                                      if (response.size() + size > MAX_RESPONSE_BYTES) return false;
                                      response.append(reinterpret_cast<const char*>(data), size);
                                      return true;
                                    });
  const bool complete = http.responseComplete() && !http.callbackAborted();
  http.end();
  return complete ? code : -2;
}

int post(const std::string& path, const std::string& body, std::string& response) {
  return request("POST", path, body, response);
}

bool shouldDefer(JsonObjectConst event) {
  const std::string type = event["event_type"] | "";
  if (type.rfind("annotation.", 0) == 0 || type.rfind("bookmark.", 0) == 0) return true;
  const std::string directive = event["directive"] | "";
  if (directive == "own" || directive == "keep_local" || directive == "equivalent") return false;
  return type == "progress.changed";
}

}  // namespace

SyncClient::RecoveryOutcome SyncClient::fetchProgress(const std::string& bookHash, bool otherDeviceOnly) {
  RecoveryOutcome result;
  if (!validRecoveryBookHash(bookHash)) return result;
  if (!BORGES_CREDENTIALS.paired()) {
    result.result = Result::NOT_PAIRED;
    return result;
  }
  if (!BORGES_QUEUE.begin()) {
    result.result = Result::STORAGE_ERROR;
    return result;
  }
  if (insufficientHeap()) {
    result.result = Result::LOW_MEMORY;
    return result;
  }
  if (!ensureTrustedClock()) {
    result.result = Result::CLOCK_ERROR;
    return result;
  }
  std::string response;
  const int code = request(
      "GET",
      "/api/sync/v2/progress/positions?book_identifier_kind=koreader_partial_md5&book_identifier_value=" + bookHash, {},
      response);
  result.httpCode = code > 0 ? code : 0;
  if (code <= 0) {
    result.result = Result::NETWORK_ERROR;
    return result;
  }
  if (code == 401 || code == 403) {
    result.result = Result::AUTH_ERROR;
    return result;
  }
  if (code != 200) {
    result.result = Result::SERVER_ERROR;
    return result;
  }
  JsonDocument document;
  if (deserializeJson(document, response)) return result;
  const auto selected = selectRecoveryPosition(document.as<JsonObjectConst>(), bookHash, otherDeviceOnly);
  if (!selected.valid) return result;
  result.found = !selected.eventJson.empty();
  if (result.found && !BORGES_QUEUE.restoreProgressCandidate(bookHash, selected.eventJson)) {
    result.result = Result::STORAGE_ERROR;
    return result;
  }
  result.result = Result::OK;
  return result;
}

SyncClient::Outcome SyncClient::syncOnce() {
  // Persist the complete remote backlog before exposing any queued local position.
  // A failed/truncated pull or more pages must never fall through to an upload.
  Outcome pulled = syncStep(false);
  if (!mayUploadAfterPull(pulled.result == Result::OK, pulled.hasMore) || pulled.queueDepth == 0) return pulled;
  Outcome exchanged = syncStep(true);
  exchanged.pulled += pulled.pulled;
  return exchanged;
}

SyncClient::Outcome SyncClient::syncStep(bool allowUpload) {
  Outcome outcome;
  if (!BORGES_CREDENTIALS.paired()) {
    outcome.result = Result::NOT_PAIRED;
    return outcome;
  }
  if (!BORGES_QUEUE.begin()) {
    outcome.result = Result::STORAGE_ERROR;
    return outcome;
  }
  outcome.queueDepth = BORGES_QUEUE.depth();
  outcome.cursor = BORGES_QUEUE.checkpoint().appliedCursor;
  if (insufficientHeap()) {
    outcome.result = Result::LOW_MEMORY;
    return outcome;
  }
  if (!ensureTrustedClock()) {
    outcome.result = Result::CLOCK_ERROR;
    return outcome;
  }

  const std::vector<PendingEvent> batch = allowUpload ? BORGES_QUEUE.nextBatch() : std::vector<PendingEvent>{};
  const bool exchanging = !batch.empty();
  if (exchanging) {
    for (const PendingEvent& event : batch) {
      if (!BORGES_QUEUE.markAttempted(event)) {
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

  const JsonObjectConst pull = exchanging ? document["pull"].as<JsonObjectConst>() : document.as<JsonObjectConst>();
  const auto validated = validatePull(pull, outcome.cursor);
  if (!validated || (exchanging && !document["acknowledgements"].is<JsonArrayConst>())) {
    outcome.result = Result::INVALID_RESPONSE;
    return outcome;
  }
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
      if (!BORGES_QUEUE.deferPulled(*serverSequence, serialized)) {
        outcome.result = Result::STORAGE_ERROR;
        return outcome;
      }
    }
    lastSeenSequence = *serverSequence;
    ++outcome.pulled;
  }

  const auto responseCursor = decimalSequence(pull["cursor"] | "");
  if (!responseCursor || *responseCursor < lastSeenSequence || *responseCursor < outcome.cursor ||
      !BORGES_QUEUE.commitAppliedCursor(*responseCursor)) {
    outcome.result = Result::INVALID_RESPONSE;
    return outcome;
  }
  outcome.cursor = *responseCursor;
  outcome.hasMore = validated->hasMore;

  if (exchanging) {
    for (JsonObjectConst acknowledgement : document["acknowledgements"].as<JsonArrayConst>()) {
      const char* id = acknowledgement["client_event_id"] | "";
      const char* sequenceText = acknowledgement["client_sequence"] | "";
      const std::string status = acknowledgement["status"] | "";
      for (const PendingEvent& event : batch) {
        if (!acknowledgmentMatches(event.id, event.sequence, id, sequenceText)) continue;
        if (status == "accepted" || status == "rejected") {
          if (!BORGES_QUEUE.retireAcknowledged(event.id, event.sequence)) {
            outcome.result = Result::STORAGE_ERROR;
            return outcome;
          }
          if (status == "accepted") {
            ++outcome.acknowledged;
          } else {
            ++outcome.rejected;
            BORGES_CREDENTIALS.setLastError(acknowledgement["rejection_code"] | "event_rejected");
          }
        }
      }
    }
  }

  if (const auto serverTime = parseRfc3339Utc(document["server_time"] | ""); serverTime.has_value()) {
    BORGES_QUEUE.setWallAnchor(*serverTime, millis());
  }
  outcome.queueDepth = BORGES_QUEUE.depth();
  if (outcome.rejected == 0) BORGES_CREDENTIALS.setLastError({});
  BORGES_CREDENTIALS.saveToFile();
  outcome.result = Result::OK;
  return outcome;
}

SyncClient::Result SyncClient::resolveSuggestion(const std::string& suggestionId, bool accept) {
  if (!BORGES_CREDENTIALS.paired() || !isUuid(suggestionId)) return Result::INVALID_RESPONSE;
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

}  // namespace borges
