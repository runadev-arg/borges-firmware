#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

namespace borges {

class CredentialStore : public PersistableStore<CredentialStore> {
 private:
  std::string baseUrl = "https://borges.runadev.com";
  std::string deviceId;
  std::string token;
  std::string pairingRequestId;
  std::string pairingNonce;
  std::string pairingUserCode;
  std::string verificationUrl;
  uint64_t pairingExpiresAt = 0;
  uint32_t pairingIntervalSeconds = 5;
  std::string lastError;
  // Last known good IPv4 of the server ("104.21.0.166") and the host it belongs
  // to, so a changed baseUrl cannot resurrect a stale address.
  std::string serverIp;
  std::string serverIpHost;

  CredentialStore() = default;
  friend class PersistableStore<CredentialStore>;

 public:
  static const char* getFilePath() { return "/.borges/borges.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool paired() const { return !deviceId.empty() && !token.empty(); }
  bool pairingPending() const { return !pairingRequestId.empty() && !pairingNonce.empty(); }
  void setPairing(std::string requestId, std::string nonce, std::string userCode, std::string verifyUrl,
                  uint64_t expiresAt, uint32_t intervalSeconds);
  void clearPairing();
  void setCredential(std::string id, std::string secret);
  void clearCredential();

  const std::string& getBaseUrl() const { return baseUrl; }
  void setBaseUrl(std::string value);
  const std::string& getDeviceId() const { return deviceId; }
  const std::string& getToken() const { return token; }
  const std::string& getPairingRequestId() const { return pairingRequestId; }
  const std::string& getPairingNonce() const { return pairingNonce; }
  const std::string& getPairingUserCode() const { return pairingUserCode; }
  const std::string& getVerificationUrl() const { return verificationUrl; }
  uint64_t getPairingExpiresAt() const { return pairingExpiresAt; }
  uint32_t getPairingIntervalSeconds() const { return pairingIntervalSeconds; }
  const std::string& getLastError() const { return lastError; }
  void setLastError(std::string value);

  const std::string& getServerIp() const { return serverIp; }
  const std::string& getServerIpHost() const { return serverIpHost; }
  void setServerIp(std::string host, std::string ip);  // both or neither
  void clearServerIp();
};

bool bootstrapBorgesServices();

}  // namespace borges

#define BORGES_CREDENTIALS borges::CredentialStore::getInstance()
