#include "BorgesPairingClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_random.h>

#include <array>
#include <cstdio>
#include <string>

#include "BorgesCredentialStore.h"
#include "BorgesNetBoot.h"
#include "BorgesTrust.h"

#ifndef BORGES_VERSION
#define BORGES_VERSION "development"
#endif

namespace borges {
namespace {

constexpr uint32_t MIN_FREE_FOR_TLS = 50000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr size_t MAX_RESPONSE_BYTES = 12 * 1024;

// ISRG Root YE from https://letsencrypt.org/certs/gen-y/root-ye.pem.
// The owned endpoint currently serves Let's Encrypt's YE2 chain. Trusting Root
// YE lets wolfSSL stop before the longer X2/X1 compatibility path.
constexpr char ISRG_ROOT_YE[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL
MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN
MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG
A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi
A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X
yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8
7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O
BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9
LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m
onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47
-----END CERTIFICATE-----
)PEM";

bool insufficientHeap() { return ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS; }

std::string base64UrlNonce() {
  std::array<uint8_t, 32> bytes{};
  esp_fill_random(bytes.data(), bytes.size());
  String encoded = base64::encode(bytes.data(), bytes.size());
  encoded.replace("+", "-");
  encoded.replace("/", "_");
  while (encoded.endsWith("=")) encoded.remove(encoded.length() - 1);
  return encoded.c_str();
}

std::string externalId() {
  std::array<char, 32> value{};
  std::snprintf(value.data(), value.size(), "x4-%012llx",
                static_cast<unsigned long long>(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));
  return value.data();
}

// One-line triage for when NEITHER SNTP NOR the HTTP Date could set the clock.
// Reports the DHCP lease plus a raw-IP TCP probe that needs no DNS at all,
// which splits "no lease" from "DNS broken but internet up" from "no egress".
// The name-resolution side of the story now comes, better, from
// netboot::Candidates::detail.
std::string clockTriage() {
  std::string detail = "ip:" + std::string(WiFi.localIP().toString().c_str());
  detail += " gw:" + std::string(WiFi.gatewayIP().toString().c_str());
  detail += " d:" + std::string(WiFi.dnsIP(0).toString().c_str());
  WiFiClient rawProbe;
  detail += rawProbe.connect(IPAddress(1, 1, 1, 1), 443, 4000) ? " raw443:ok" : " raw443:fail";
  rawProbe.stop();
  return detail;
}

// With no reachable UART in the field, the only way to read WHY a pairing run
// died is the credential file on the SD card (/.borges/borges.json,
// lastError). One failed run is one or two sub-kilobyte writes; never call this
// from a fast poll loop.
void persistFailure() {
  std::string detail = PairingClient::lastErrorDetail;
  if (detail.empty() && PairingClient::lastHttpCode > 0) {
    detail = "http " + std::to_string(PairingClient::lastHttpCode);
  }
  if (detail.empty()) return;
  BORGES_CREDENTIALS.setLastError(detail);
  BORGES_CREDENTIALS.saveToFile();
}

void persistFailure(const char* detail) {
  PairingClient::lastErrorDetail = detail;
  persistFailure();
}

int postJson(const std::string& path, const std::string& body, std::string& response) {
  netboot::WifiFullPowerScope wifiFullPower;
  PairingClient::lastErrorDetail.clear();
  if (insufficientHeap()) return -3;

  // The URL is validated BEFORE the clock: the HTTP Date bootstrap needs the
  // host both to resolve it and for the Host header.
  const std::string baseUrl = BORGES_CREDENTIALS.getBaseUrl();
  if (baseUrl.rfind("https://", 0) != 0) {
    PairingClient::lastErrorDetail = "badurl:" + baseUrl.substr(0, 24);
    return -4;
  }
  const std::string host = netboot::hostFromBaseUrl(baseUrl);
  if (host.empty()) {
    PairingClient::lastErrorDetail = "badurl:host";
    return -4;
  }

  const netboot::Candidates candidates = netboot::resolveServer(host.c_str());

  std::string clockDetail;
  if (!ensureTrustedClock(&clockDetail)) {
    PairingClient::lastErrorDetail = "clock " + clockTriage() + " " + clockDetail;
    return -5;
  }
  if (candidates.count == 0) {
    PairingClient::lastErrorDetail = "resolve:none " + candidates.detail;
    return -1;
  }

  // Candidates are tried in order. The next one is only worth a retry when the
  // failure was at TCP level (dead candidate: fails in <=3 s). A TLS failure
  // means there IS a TLS server on the other side: switching IP fixes nothing
  // and would cost another 15 s handshake.
  int status = -1;
  for (uint8_t attempt = 0; attempt < candidates.count && attempt < 3; ++attempt) {
    freeink::SecureHttpClient http;
    http.setCACert(rootCertificate());
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(std::string("Borges-Borges/") + BORGES_VERSION);
    if (!http.begin(baseUrl + path)) {
      PairingClient::lastErrorDetail = "http_begin";
      return -1;
    }
    http.setServerAddress(candidates.ip[attempt]);  // begin() FIRST, pin AFTER
    http.addHeader("Accept", "application/json");
    http.addHeader("Content-Type", "application/json");

    response.clear();
    response.reserve(2048);
    status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                              [&response](const uint8_t* data, size_t size) {
                                if (response.size() + size > MAX_RESPONSE_BYTES) return false;
                                response.append(reinterpret_cast<const char*>(data), size);
                                return true;
                              });
    const bool complete = http.responseComplete() && !http.callbackAborted();
    const bool aborted = http.callbackAborted();
    const std::string transportDetail = http.lastErrorDetail();
    http.end();

    if (status > 0) {
      if (!complete) {
        PairingClient::lastErrorDetail = aborted ? "resp_too_big" : "resp_incomplete";
        return -2;
      }
      // That candidate works: cache it for the next boot.
      netboot::rememberServerIp(host.c_str(), candidates.ip[attempt]);
      return status;
    }

    PairingClient::lastErrorDetail = transportDetail.empty() ? ("http:" + std::to_string(status)) : transportDetail;
    // A dead candidate (TCP/DNS) justifies a retry, and so does a certificate
    // name mismatch (wolfSSL DOMAIN_NAME_MISMATCH, -322): that is the exact
    // signature of dialing a WRONG address that still runs a TLS server --
    // e.g. a DNS answer poisoned by a captive portal -- and it fails fast,
    // right after the peer's certificate arrives. Other TLS failures mean the
    // right server is unhappy; switching IP fixes nothing there.
    if (transportDetail.rfind("tcp", 0) != 0 && transportDetail.rfind("dns", 0) != 0 &&
        transportDetail.rfind("tls:-322", 0) != 0) {
      break;
    }
    PairingClient::lastErrorDetail += " try" + std::to_string(attempt + 1);
  }
  PairingClient::lastErrorDetail += " " + candidates.detail;
  return status > 0 ? -2 : -1;
}

PairingClient::Result transportResult(int status) {
  PairingClient::lastHttpCode = status > 0 ? status : 0;
  persistFailure();
  if (status == -3) return PairingClient::Result::LOW_MEMORY;
  if (status == -5) return PairingClient::Result::CLOCK_ERROR;
  if (status <= 0) return PairingClient::Result::NETWORK_ERROR;
  if (status >= 500 || status == 429) return PairingClient::Result::SERVER_ERROR;
  return PairingClient::Result::INVALID_RESPONSE;
}

}  // namespace

const char* rootCertificate() { return ISRG_ROOT_YE; }

int PairingClient::lastHttpCode = 0;
std::string PairingClient::lastErrorDetail;

PairingClient::Result PairingClient::request(const char* deviceName) {
  JsonDocument request;
  const std::string nonce = base64UrlNonce();
  request["device_nonce"] = nonce;
  request["device_name"] = deviceName;
  request["platform"] = "crosspoint-x4";
  request["external_id"] = externalId();
  request["firmware_version"] = BORGES_VERSION;
  request["client_version"] = "borges-sync-v2";
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
  if (deserializeJson(result, response)) {
    persistFailure("bad_json");
    return Result::INVALID_RESPONSE;
  }
  const std::string requestId = result["request_id"] | "";
  const std::string userCode = result["user_code"] | "";
  std::string verificationUrl = result["verification_url"] | "";
  if (!verificationUrl.empty() && verificationUrl.front() == '/') {
    verificationUrl = BORGES_CREDENTIALS.getBaseUrl() + verificationUrl;
  }
  if (requestId.empty() || userCode.empty() || verificationUrl.empty()) {
    persistFailure("bad_fields");
    return Result::INVALID_RESPONSE;
  }

  BORGES_CREDENTIALS.setPairing(requestId, nonce, userCode, verificationUrl, 0, result["interval"] | 5U);
  if (!BORGES_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
  LOG_INF("BORGES", "Pairing request created; code is available in device UI");
  return Result::OK;
}

PairingClient::Result PairingClient::pollAndClaim() {
  if (!BORGES_CREDENTIALS.pairingPending()) return Result::NO_REQUEST;
  JsonDocument statusRequest;
  statusRequest["request_id"] = BORGES_CREDENTIALS.getPairingRequestId();
  statusRequest["device_nonce"] = BORGES_CREDENTIALS.getPairingNonce();
  std::string body;
  serializeJson(statusRequest, body);

  std::string response;
  int status = postJson("/api/devices/pairing/status", body, response);
  lastHttpCode = status > 0 ? status : 0;
  if (status != 200) return transportResult(status);
  JsonDocument statusDoc;
  if (deserializeJson(statusDoc, response)) {
    persistFailure("bad_json");
    return Result::INVALID_RESPONSE;
  }
  const std::string pairingStatus = statusDoc["status"] | "";
  if (pairingStatus == "pending") return Result::PENDING;
  if (pairingStatus == "rejected" || pairingStatus == "expired") {
    BORGES_CREDENTIALS.clearPairing();
    if (!BORGES_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
    return pairingStatus == "rejected" ? Result::REJECTED : Result::EXPIRED;
  }
  if (pairingStatus != "approved") {
    persistFailure("bad_fields");
    return Result::INVALID_RESPONSE;
  }

  response.clear();
  status = postJson("/api/devices/pairing/claim", body, response);
  lastHttpCode = status > 0 ? status : 0;
  if (status != 200) return transportResult(status);
  JsonDocument claim;
  if (deserializeJson(claim, response)) {
    persistFailure("bad_json");
    return Result::INVALID_RESPONSE;
  }
  const std::string deviceId = claim["credential"]["username"] | "";
  const std::string token = claim["credential"]["token"] | "";
  if (deviceId.empty() || token.size() < 32) {
    persistFailure("bad_fields");
    return Result::INVALID_RESPONSE;
  }

  BORGES_CREDENTIALS.setCredential(deviceId, token);
  if (!BORGES_CREDENTIALS.saveToFile()) return Result::PERSISTENCE_ERROR;
  if (!bootstrapBorgesServices()) return Result::PERSISTENCE_ERROR;
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

}  // namespace borges
