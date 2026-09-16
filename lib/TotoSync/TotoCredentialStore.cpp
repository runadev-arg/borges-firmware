#include "TotoCredentialStore.h"

#include <ObfuscationUtils.h>

#include <algorithm>
#include <utility>

namespace toto {
namespace {

constexpr uint8_t CONFIG_VERSION = 2;

std::string decodeSecret(JsonVariantConst doc, const char* obfuscatedKey, const char* legacyKey, bool& needsResave) {
  bool ok = false;
  std::string value = obfuscation::deobfuscateFromBase64(doc[obfuscatedKey] | "", &ok);
  if (!ok) {
    value = doc[legacyKey] | "";
    needsResave = needsResave || !value.empty();
  }
  return value;
}

std::string stripTrailingSlashes(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

}  // namespace

void CredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["baseUrl"] = baseUrl;
  doc["deviceId"] = deviceId;
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["accountUsername"] = accountUsername;
  doc["accountKey"] = accountKey;
  doc["tokenExpiresAt"] = tokenExpiresAt;
  doc["tokenRenewAfter"] = tokenRenewAfter;
  doc["pairingRequestId"] = pairingRequestId;
  doc["pairingNonce_obf"] = obfuscation::obfuscateToBase64(pairingNonce);
  doc["pairingUserCode"] = pairingUserCode;
  doc["verificationUrl"] = verificationUrl;
  doc["pairingExpiresAt"] = pairingExpiresAt;
  doc["pairingIntervalSeconds"] = pairingIntervalSeconds;
  doc["lastError"] = lastError;
}

bool CredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  baseUrl = stripTrailingSlashes(doc["baseUrl"] | "https://highlights.runadev.com");
  if (baseUrl.rfind("https://", 0) != 0) {
    baseUrl = "https://highlights.runadev.com";
    needsResave = true;
  }
  deviceId = doc["deviceId"] | "";
  token = decodeSecret(doc, "token_obf", "token", needsResave);
  accountUsername = doc["accountUsername"] | "";
  accountKey = doc["accountKey"] | "";
  // A file written before the account era holds a credential but no key.
  // Adopting a device sentinel keeps the first sign-in classified as a switch,
  // which is what purges whatever the previous account left behind.
  if (accountKey.empty() && !deviceId.empty()) {
    accountKey = "device:" + deviceId;
    needsResave = true;
  }
  tokenExpiresAt = doc["tokenExpiresAt"] | static_cast<uint64_t>(0);
  tokenRenewAfter = doc["tokenRenewAfter"] | static_cast<uint64_t>(0);
  pairingRequestId = doc["pairingRequestId"] | "";
  pairingNonce = decodeSecret(doc, "pairingNonce_obf", "pairingNonce", needsResave);
  pairingUserCode = doc["pairingUserCode"] | "";
  verificationUrl = doc["verificationUrl"] | "";
  pairingExpiresAt = doc["pairingExpiresAt"] | static_cast<uint64_t>(0);
  pairingIntervalSeconds = std::clamp<uint32_t>(doc["pairingIntervalSeconds"] | 5U, 2U, 60U);
  lastError = doc["lastError"] | "";

  if ((doc["cfgVersion"] | 0U) < CONFIG_VERSION) needsResave = true;
  if (needsResave) requestResave();
  return true;
}

void CredentialStore::setPairing(std::string requestId, std::string nonce, std::string userCode, std::string verifyUrl,
                                 uint64_t expiresAt, uint32_t intervalSeconds) {
  pairingRequestId = std::move(requestId);
  pairingNonce = std::move(nonce);
  pairingUserCode = std::move(userCode);
  verificationUrl = std::move(verifyUrl);
  pairingExpiresAt = expiresAt;
  pairingIntervalSeconds = std::clamp<uint32_t>(intervalSeconds, 2U, 60U);
  lastError.clear();
}

void CredentialStore::clearPairing() {
  pairingRequestId.clear();
  pairingNonce.clear();
  pairingUserCode.clear();
  verificationUrl.clear();
  pairingExpiresAt = 0;
  pairingIntervalSeconds = 5;
}

void CredentialStore::setCredential(std::string id, std::string secret) {
  deviceId = std::move(id);
  token = std::move(secret);
  // Code pairing never learns which account approved it, so the key is tied to
  // the device instead of to a name.
  accountUsername.clear();
  accountKey = "device:" + deviceId;
  tokenExpiresAt = 0;
  tokenRenewAfter = 0;
  clearPairing();
  lastError.clear();
}

void CredentialStore::setSession(const DeviceSession& session) {
  deviceId = session.deviceId;
  token = session.token;
  accountUsername = session.accountUsername;
  accountKey = accountFingerprint(session.accountUsername);
  if (accountKey.empty()) accountKey = "device:" + deviceId;
  tokenExpiresAt = session.expiresAt;
  tokenRenewAfter = session.renewAfter;
  clearPairing();
  lastError.clear();
}

void CredentialStore::clearCredential() {
  deviceId.clear();
  token.clear();
  accountUsername.clear();
  accountKey.clear();
  tokenExpiresAt = 0;
  tokenRenewAfter = 0;
  lastError.clear();
}

void CredentialStore::setBaseUrl(std::string value) {
  value = stripTrailingSlashes(std::move(value));
  if (value.rfind("https://", 0) == 0) baseUrl = std::move(value);
}

void CredentialStore::setLastError(std::string value) {
  constexpr size_t MAX_ERROR_LENGTH = 160;
  if (value.size() > MAX_ERROR_LENGTH) value.resize(MAX_ERROR_LENGTH);
  lastError = std::move(value);
}

}  // namespace toto
