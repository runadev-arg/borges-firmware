#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <cmath>
#include <string>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncProtocol.h"
#include "TotoCredentialStore.h"
#include "TotoNetBoot.h"
#include "TotoTrust.h"

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastProtocolCode = 0;
std::string KOReaderSyncClient::lastRequestId;
std::string KOReaderSyncClient::lastEndpoint;
std::string KOReaderSyncClient::lastTransportError;

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// KOSync's TLS-1.3 servers can't be reached through the precompiled system
// mbedTLS (TLS 1.3 is stubbed out), so requests run over wolfSSL via
// SecureHttpClient. The handshake still needs working heap; gate on it. wolfSSL's
// footprint is smaller than mbedTLS's old ~48KB peak, but keep a conservative
// floor. Check both total free heap and largest contiguous block so fragmented
// heap does not fall through into a failed TLS allocation path.
// MEMFIX-PORT: TLS heap gate; portable
// Field data (July 2026): launching sync from a reader session lands at
// 51.9-58.2 KB free / 42-53 KB maxAlloc after WiFi comes up. wolfSSL handles
// allocation failure by returning MEMORY_E (no abort under -fno-exceptions),
// so an optimistic attempt degrades to the same clean "sync failed" as the
// gate — the gate only needs to keep out states where a doomed handshake
// would waste tens of seconds, not guarantee success.
//
// Free and largest-block have separate requirements: with SP ECC
// (WOLFSSL_HAVE_SP_ECC) the handshake's crypto uses fixed 256-bit arrays, so
// the largest single TLS allocation is the ~17 KB wolfSSL record buffer, not
// a run of fast-math bignums. A handshake was measured succeeding inside a
// 43 KB largest block; requiring 50 KB contiguous refused syncs that fit.
constexpr uint32_t MIN_FREE_FOR_TLS = 50000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;
constexpr size_t MAX_DIAGNOSTIC_BYTES = 96;

std::string boundedDiagnostic(std::string value) {
  if (value.size() > MAX_DIAGNOSTIC_BYTES) value.resize(MAX_DIAGNOSTIC_BYTES);
  return value;
}

void resetDiagnostics(const char* endpoint) {
  KOReaderSyncClient::lastHttpCode = 0;
  KOReaderSyncClient::lastProtocolCode = 0;
  KOReaderSyncClient::lastRequestId.clear();
  KOReaderSyncClient::lastEndpoint = endpoint;
  KOReaderSyncClient::lastTransportError.clear();
}

void captureDiagnostics(freeink::SecureHttpClient& http, int httpCode, const std::string& body) {
  KOReaderSyncClient::lastHttpCode = httpCode;
  KOReaderSyncClient::lastProtocolCode = parseKOReaderProtocolCode(body);
  KOReaderSyncClient::lastRequestId = boundedDiagnostic(http.getHeader("x-request-id"));
  KOReaderSyncClient::lastTransportError = boundedDiagnostic(http.lastErrorDetail());
  LOG_INF("KOSync", "endpoint=%s http=%d code=%d request_id=%s transport=%s", KOReaderSyncClient::lastEndpoint.c_str(),
          httpCode, KOReaderSyncClient::lastProtocolCode,
          KOReaderSyncClient::lastRequestId.empty() ? "-" : KOReaderSyncClient::lastRequestId.c_str(),
          KOReaderSyncClient::lastTransportError.empty() ? "-" : KOReaderSyncClient::lastTransportError.c_str());
}

class Request {
 public:
  bool begin(const std::string& url) {
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(std::string("CrossPoint-KOSync/") + CROSSPOINT_VERSION);

    const std::string baseUrl = KOREADER_STORE.getBaseUrl();
    if (baseUrl.rfind("https://", 0) == 0) {
      if (TOTO_CREDENTIALS.paired()) {
        std::string clockDetail;
        if (!toto::ensureTrustedClock(&clockDetail)) {
          KOReaderSyncClient::lastTransportError =
              boundedDiagnostic(clockDetail.empty() ? "trusted-clock bootstrap failed" : clockDetail);
          LOG_ERR("KOSync", "Trusted TLS clock unavailable: %s", KOReaderSyncClient::lastTransportError.c_str());
          return false;
        }
        http.setCACert(toto::rootCertificate());
      } else {
        // Preserve standalone third-party KOSync compatibility when no Toto
        // pairing exists to provide a pinned certificate and trusted clock.
        http.setInsecure();
      }
      const std::string host = toto::netboot::hostFromBaseUrl(baseUrl);
      if (host.empty()) return false;
      const toto::netboot::Candidates candidates = toto::netboot::resolveServer(host.c_str());
      if (candidates.count == 0 || !http.begin(url)) return false;
      http.setServerAddress(candidates.ip[0]);
      return true;
    }
    return http.begin(url);
  }

  toto::netboot::WifiFullPowerScope wifiFullPower;
  freeink::SecureHttpClient http;
};

KOReaderSyncClient::Error mappedError(KOReaderSyncOperation operation, int httpCode, const std::string& body) {
  switch (classifyKOReaderResponse(operation, httpCode, body)) {
    case KOReaderSyncResponse::OK:
      return KOReaderSyncClient::OK;
    case KOReaderSyncResponse::NOT_FOUND:
      return KOReaderSyncClient::NOT_FOUND;
    case KOReaderSyncResponse::AUTH_FAILED:
      return KOReaderSyncClient::AUTH_FAILED;
    case KOReaderSyncResponse::ACCESS_DENIED:
      return KOReaderSyncClient::ACCESS_DENIED;
    case KOReaderSyncResponse::INVALID_REQUEST:
      return KOReaderSyncClient::INVALID_REQUEST;
    case KOReaderSyncResponse::USER_EXISTS:
      return KOReaderSyncClient::USER_EXISTS;
    default:
      return KOReaderSyncClient::SERVER_ERROR;
  }
}

// Apply the shared KOSync auth headers after begin(). x-auth-* is the native
// KOSync scheme; Basic auth is added for Calibre-Web-Automated compatibility.
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  resetDiagnostics("GET users/auth");
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  Request request;
  if (!request.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(request.http);
  const int httpCode = request.http.GET();
  const std::string body = httpCode > 0 ? request.http.getString() : "";
  captureDiagnostics(request.http, httpCode, body);
  request.http.end();
  return httpCode <= 0 ? NETWORK_ERROR : mappedError(KOReaderSyncOperation::AUTHENTICATE, httpCode, body);
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  resetDiagnostics("POST users/create");
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  Request request;
  if (!request.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  request.http.addHeader("Accept", "application/vnd.koreader.v1+json");
  request.http.addHeader("Content-Type", "application/json");
  const int httpCode = request.http.sendRequest("POST", body);
  const std::string response = httpCode > 0 ? request.http.getString() : "";
  captureDiagnostics(request.http, httpCode, response);
  request.http.end();
  return httpCode <= 0 ? NETWORK_ERROR : mappedError(KOReaderSyncOperation::CREATE_USER, httpCode, response);
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  resetDiagnostics("GET syncs/progress/:document");
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (!isValidKOReaderDocumentHash(documentHash)) {
    lastProtocolCode = 2003;
    LOG_ERR("KOSync", "Refusing invalid document hash");
    return INVALID_REQUEST;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  Request request;
  if (!request.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(request.http);
  const int httpCode = request.http.GET();
  const std::string body = httpCode > 0 ? request.http.getString() : "";
  captureDiagnostics(request.http, httpCode, body);
  request.http.end();

  if (httpCode <= 0) {
    return NETWORK_ERROR;
  }

  const Error responseError = mappedError(KOReaderSyncOperation::GET_PROGRESS, httpCode, body);
  if (responseError == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);

    if (error || !doc["progress"].is<const char*>() || !doc["percentage"].is<float>()) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    // Extended crosspoint-sync field; absent on plain kosync servers.
    outProgress.position.reset();
    const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
    if (!pos.isNull()) {
      KOReaderRichPosition rich;
      rich.pctQ = pos["pctQ"].as<uint32_t>();
      rich.spineIndex = pos["spine"].as<uint16_t>();
      rich.pageNumber = pos["page"].as<uint16_t>();
      const uint16_t pages = pos["pages"].as<uint16_t>();
      rich.totalPages = pages > 0 ? pages : 1;
      const uint16_t para = pos["para"].as<uint16_t>();
      if (para > 0) rich.paragraphIndex = para;
      rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
      LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
              rich.totalPages, para);
      outProgress.position = std::move(rich);
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }
  return responseError;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  resetDiagnostics("PUT syncs/progress");
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (!isValidKOReaderDocumentHash(progress.document) || progress.progress.empty() || progress.progress.size() > 4096 ||
      !std::isfinite(progress.percentage) || progress.percentage < 0.0f || progress.percentage > 1.0f) {
    lastProtocolCode = 2003;
    LOG_ERR("KOSync", "Refusing invalid local progress payload");
    return INVALID_REQUEST;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value()) {
    // Extended crosspoint-sync field; kosync servers ignore unknown keys.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Prepared progress payload: %u bytes", static_cast<unsigned>(body.size()));

  Request request;
  if (!request.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(request.http);
  request.http.addHeader("Content-Type", "application/json");
  const int httpCode = request.http.sendRequest("PUT", body);
  const std::string response = httpCode > 0 ? request.http.getString() : "";
  captureDiagnostics(request.http, httpCode, response);
  request.http.end();
  return httpCode <= 0 ? NETWORK_ERROR : mappedError(KOReaderSyncOperation::PUT_PROGRESS, httpCode, response);
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    case USER_EXISTS:
      return "Username already exists";
    case ACCESS_DENIED:
      return "Sync access denied; pair this reader again";
    case INVALID_REQUEST:
      return "Invalid sync data";
    default:
      return "Unknown error";
  }
}
