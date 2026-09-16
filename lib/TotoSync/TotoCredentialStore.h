#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

#include "TotoDeviceLogin.h"

namespace toto {

class CredentialStore : public PersistableStore<CredentialStore> {
 private:
  std::string baseUrl = "https://highlights.runadev.com";
  std::string deviceId;
  std::string token;
  // Display name of the signed-in account, empty when the reader arrived by
  // code pairing. The account password is never a field here, on disk or in
  // memory: it is spent on the login request and dropped.
  std::string accountUsername;
  // Comparison key for "is this the same account as last time". Login stores
  // the folded username; code pairing stores a per-device sentinel, so a later
  // sign-in still reads as a switch and nothing from the old account leaks.
  std::string accountKey;
  uint64_t tokenExpiresAt = 0;
  uint64_t tokenRenewAfter = 0;
  // Wall-clock second of the last sync the server accepted. Shown to the
  // reader as "last sync", and account-scoped: it is reset with the
  // credential, so a new account never inherits the old one's success.
  uint64_t lastSyncAt = 0;
  std::string pairingRequestId;
  std::string pairingNonce;
  std::string pairingUserCode;
  std::string verificationUrl;
  uint64_t pairingExpiresAt = 0;
  uint32_t pairingIntervalSeconds = 5;
  std::string lastError;

  CredentialStore() = default;
  friend class PersistableStore<CredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/toto.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool paired() const { return !deviceId.empty() && !token.empty(); }
  bool pairingPending() const { return !pairingRequestId.empty() && !pairingNonce.empty(); }
  void setPairing(std::string requestId, std::string nonce, std::string userCode, std::string verifyUrl,
                  uint64_t expiresAt, uint32_t intervalSeconds);
  void clearPairing();
  void setCredential(std::string id, std::string secret);
  void setSession(const DeviceSession& session);
  void clearCredential();
  SessionState state(uint64_t nowUnixSeconds) const {
    return sessionState(nowUnixSeconds, tokenRenewAfter, tokenExpiresAt, !token.empty());
  }

  const std::string& getBaseUrl() const { return baseUrl; }
  void setBaseUrl(std::string value);
  const std::string& getDeviceId() const { return deviceId; }
  const std::string& getToken() const { return token; }
  const std::string& getAccountUsername() const { return accountUsername; }
  const std::string& getAccountKey() const { return accountKey; }
  uint64_t getTokenExpiresAt() const { return tokenExpiresAt; }
  uint64_t getTokenRenewAfter() const { return tokenRenewAfter; }
  uint64_t getLastSyncAt() const { return lastSyncAt; }
  void setLastSyncAt(uint64_t unixSeconds) { lastSyncAt = unixSeconds; }
  const std::string& getPairingRequestId() const { return pairingRequestId; }
  const std::string& getPairingNonce() const { return pairingNonce; }
  const std::string& getPairingUserCode() const { return pairingUserCode; }
  const std::string& getVerificationUrl() const { return verificationUrl; }
  uint64_t getPairingExpiresAt() const { return pairingExpiresAt; }
  uint32_t getPairingIntervalSeconds() const { return pairingIntervalSeconds; }
  const std::string& getLastError() const { return lastError; }
  void setLastError(std::string value);
};

bool bootstrapCrossPointServices();
// Undoes the bootstrap: drops the Toto catalogue entry and the KOSync
// credentials so a signed-out reader cannot keep reaching the old account.
void teardownCrossPointServices();

}  // namespace toto

#define TOTO_CREDENTIALS toto::CredentialStore::getInstance()
