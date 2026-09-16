#include "TotoPairingClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <base64.h>
#include <esp_random.h>

#include <array>
#include <cstdio>
#include <string>

#include "TotoCredentialStore.h"
#include "TotoDeviceLogin.h"
#include "TotoHttp.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace {

constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr size_t MAX_RESPONSE_BYTES = 12 * 1024;

std::string base64UrlNonce() {
  std::array<uint8_t, 32> bytes{};
  esp_fill_random(bytes.data(), bytes.size());
  String encoded = base64::encode(bytes.data(), bytes.size());
  encoded.replace("+", "-");
  encoded.replace("/", "_");
  while (encoded.endsWith("=")) encoded.remove(encoded.length() - 1);
  return encoded.c_str();
}

int postJson(const char* path, const std::string& body, std::string& response) {
  HttpRequest request;
  request.path = path;
  request.body = &body;
  request.timeoutMs = HTTP_TIMEOUT_MS;
  request.maxResponseBytes = MAX_RESPONSE_BYTES;
  return toto::postJson(request, response);
}

PairingClient::Result transportResult(int status) {
  PairingClient::lastHttpCode = status > 0 ? status : 0;
  if (status == HTTP_LOW_MEMORY) return PairingClient::Result::LOW_MEMORY;
  if (status == HTTP_CLOCK_ERROR) return PairingClient::Result::CLOCK_ERROR;
  if (status <= 0) return PairingClient::Result::NETWORK_ERROR;
  if (status >= 500 || status == 429) return PairingClient::Result::SERVER_ERROR;
  return PairingClient::Result::INVALID_RESPONSE;
}

}  // namespace

int PairingClient::lastHttpCode = 0;

PairingClient::Result PairingClient::request(const char* deviceName) {
  JsonDocument request;
  const std::string nonce = base64UrlNonce();
  request["device_nonce"] = nonce;
  request["device_name"] = deviceName;
  request["platform"] = DEVICE_PLATFORM;
  request["external_id"] = deviceExternalId(ESP.getEfuseMac());
  request["firmware_version"] = CROSSPOINT_VERSION;
  request["client_version"] = "toto-sync-v2";
  request["protocol_version"] = 2;
  JsonArray scopes = request["requested_scopes"].to<JsonArray>();
  scopes.add("device:self");
  scopes.add("sync:v2");
  scopes.add("library:read");
  scopes.add("kosync");
  JsonObject capabilities = request["capabilities"].to<JsonObject>();
  capabilities["atomic_outbox"] = true;
  capabilities["dual_checkpoint"] = true;
  capabilities["opds"] = true;
  capabilities["kosync"] = true;
  capabilities["hardware_rtc"] = false;

  std::string body;
  serializeJson(request, body);
  std::string response;
  const int status = postJson("/api/devices/pairing/request", body, response);
  lastHttpCode = status > 0 ? status : 0;
  if (status != 201) return transportResult(status);

  JsonDocument result;
  if (deserializeJson(result, response)) return Result::INVALID_RESPONSE;
  const std::string requestId = result["request_id"] | "";
  const std::string userCode = result["user_code"] | "";
  std::string verificationUrl = result["verification_url"] | "";
  if (!verificationUrl.empty() && verificationUrl.front() == '/') {
    verificationUrl = TOTO_CREDENTIALS.getBaseUrl() + verificationUrl;
  }
  if (requestId.empty() || userCode.empty() || verificationUrl.empty()) return Result::INVALID_RESPONSE;

  TOTO_CREDENTIALS.setPairing(requestId, nonce, userCode, verificationUrl, 0, result["interval"] | 5U);
  if (!TOTO_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
  LOG_INF("TOTO", "Pairing request created; code is available in device UI");
  return Result::OK;
}

PairingClient::Result PairingClient::pollAndClaim() {
  if (!TOTO_CREDENTIALS.pairingPending()) return Result::NO_REQUEST;
  JsonDocument statusRequest;
  statusRequest["request_id"] = TOTO_CREDENTIALS.getPairingRequestId();
  statusRequest["device_nonce"] = TOTO_CREDENTIALS.getPairingNonce();
  std::string body;
  serializeJson(statusRequest, body);

  std::string response;
  int status = postJson("/api/devices/pairing/status", body, response);
  lastHttpCode = status > 0 ? status : 0;
  if (status != 200) return transportResult(status);
  JsonDocument statusDoc;
  if (deserializeJson(statusDoc, response)) return Result::INVALID_RESPONSE;
  const std::string pairingStatus = statusDoc["status"] | "";
  if (pairingStatus == "pending") return Result::PENDING;
  if (pairingStatus == "rejected" || pairingStatus == "expired") {
    TOTO_CREDENTIALS.clearPairing();
    if (!TOTO_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
    return pairingStatus == "rejected" ? Result::REJECTED : Result::EXPIRED;
  }
  if (pairingStatus != "approved") return Result::INVALID_RESPONSE;

  response.clear();
  status = postJson("/api/devices/pairing/claim", body, response);
  lastHttpCode = status > 0 ? status : 0;
  if (status != 200) return transportResult(status);
  JsonDocument claim;
  if (deserializeJson(claim, response)) return Result::INVALID_RESPONSE;
  const std::string deviceId = claim["credential"]["username"] | "";
  const std::string token = claim["credential"]["token"] | "";
  if (deviceId.empty() || token.size() < 32) return Result::INVALID_RESPONSE;

  TOTO_CREDENTIALS.setCredential(deviceId, token);
  if (!TOTO_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
  if (!bootstrapCrossPointServices()) return Result::PERSISTENCE_ERROR;
  return Result::PAIRED;
}

const char* PairingClient::resultName(Result result) {
  switch (result) {
    case Result::OK:
      return "ok";
    case Result::PENDING:
      return "pending";
    case Result::PAIRED:
      return "paired";
    case Result::REJECTED:
      return "rejected";
    case Result::EXPIRED:
      return "expired";
    case Result::NO_REQUEST:
      return "no_request";
    case Result::LOW_MEMORY:
      return "low_memory";
    case Result::CLOCK_ERROR:
      return "clock_error";
    case Result::NETWORK_ERROR:
      return "network_error";
    case Result::SERVER_ERROR:
      return "server_error";
    case Result::INVALID_RESPONSE:
      return "invalid_response";
    case Result::PERSISTENCE_ERROR:
      return "persistence_error";
  }
  return "unknown";
}

}  // namespace toto
