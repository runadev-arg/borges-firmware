#include "TotoPairingClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_random.h>

#include <array>
#include <cstdio>
#include <string>

#include "TotoCredentialStore.h"
#include "TotoTrust.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace {

constexpr uint32_t MIN_FREE_FOR_TLS = 50000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr size_t MAX_RESPONSE_BYTES = 12 * 1024;

// ISRG Root X1 from https://letsencrypt.org/certs/isrgrootx1.pem.
// The owned endpoint serves Let's Encrypt's default compatibility chain to
// this trust anchor. Pairing never falls back to setInsecure().
constexpr char ISRG_ROOT_X1[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
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

int postJson(const std::string& path, const std::string& body, std::string& response) {
  if (insufficientHeap()) return -3;
  if (!ensureTrustedClock()) return -5;
  const std::string& baseUrl = TOTO_CREDENTIALS.getBaseUrl();
  if (baseUrl.rfind("https://", 0) != 0) return -4;

  freeink::SecureHttpClient http;
  http.setCACert(rootCertificate());
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setUserAgent(std::string("CrossPoint-Toto/") + CROSSPOINT_VERSION);
  if (!http.begin(baseUrl + path)) return -1;
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");

  response.clear();
  response.reserve(2048);
  const int status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                      [&response](const uint8_t* data, size_t size) {
                                        if (response.size() + size > MAX_RESPONSE_BYTES) return false;
                                        response.append(reinterpret_cast<const char*>(data), size);
                                        return true;
                                      });
  const bool complete = http.responseComplete() && !http.callbackAborted();
  http.end();
  return complete ? status : -2;
}

PairingClient::Result transportResult(int status) {
  PairingClient::lastHttpCode = status > 0 ? status : 0;
  if (status == -3) return PairingClient::Result::LOW_MEMORY;
  if (status == -5) return PairingClient::Result::CLOCK_ERROR;
  if (status <= 0) return PairingClient::Result::NETWORK_ERROR;
  if (status >= 500 || status == 429) return PairingClient::Result::SERVER_ERROR;
  return PairingClient::Result::INVALID_RESPONSE;
}

}  // namespace

const char* rootCertificate() { return ISRG_ROOT_X1; }

int PairingClient::lastHttpCode = 0;

PairingClient::Result PairingClient::request(const char* deviceName) {
  JsonDocument request;
  const std::string nonce = base64UrlNonce();
  request["device_nonce"] = nonce;
  request["device_name"] = deviceName;
  request["platform"] = "crosspoint-x4";
  request["external_id"] = externalId();
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
