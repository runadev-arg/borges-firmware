#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Client half of the "Login de lector v1" contract published by the hub.
//
//   repo:   runadev-arg/highlights-kobo (Cinabrio)
//   file:   docs/device-login-v1.md
//   commit: 74f56186b8cac5a13a0dafec0d1f568c97451b7b
//
// Everything here is pure: no Arduino, no ArduinoJson, no filesystem. The
// firmware half lives in TotoLoginClient; the host tests replay the contract
// fixtures published alongside the document through these functions.
namespace toto {

// Pinned so a silent contract drift shows up in the diff instead of in the
// field. The fixtures under test/toto_device_login are copied from this commit.
inline constexpr char DEVICE_LOGIN_CONTRACT_SHA[] = "74f56186b8cac5a13a0dafec0d1f568c97451b7b";
inline constexpr char DEVICE_LOGIN_PATH[] = "/api/devices/v1/login";
inline constexpr char DEVICE_PLATFORM[] = "crosspoint-x4";

// The contract's closed error set. UNKNOWN covers a code this firmware has not
// been taught yet, which is treated as "try again later", never as success.
enum class LoginError : uint8_t {
  NONE,
  INVALID_CREDENTIALS,
  EMAIL_NOT_VERIFIED,
  DEVICE_REVOKED,
  TLS_REQUIRED,
  SCOPE_NOT_GRANTED,
  DEVICE_LIMIT_REACHED,
  INVALID_REQUEST,
  OWNER_NOT_ACCEPTED,
  RATE_LIMITED,
  LOGIN_UNAVAILABLE,
  UNKNOWN,
};

// What the reader should tell the person to do next. Every failure resolves to
// exactly one of these so no screen can end on a bare "failed".
enum class NextStep : uint8_t {
  NONE,
  REENTER_CREDENTIALS,
  VERIFY_EMAIL,
  RELINK_FROM_WEB,
  USE_HTTPS,
  FREE_A_DEVICE_SLOT,
  FIX_REQUEST,
  WAIT_AND_RETRY,
  RECONNECT,
};

// The 200 body, reduced to what the reader actually uses. The account password
// is deliberately absent: it is never stored, not even in memory past the
// request that spends it.
struct DeviceSession {
  std::string accountUsername;
  std::string deviceId;  // credential.username, also the Basic/KOSync user
  std::string token;
  // Safe to log and to show: the hub publishes it precisely so a reader can
  // name a credential without handling the credential.
  std::string tokenPrefix;
  // legacy.opds.username, which the contract says equals the device id. Kept
  // so a response that contradicts itself can be rejected instead of half-used.
  std::string opdsUsername;
  uint64_t expiresAt = 0;
  uint64_t renewAfter = 0;
  bool grantsSyncV2 = false;
  bool grantsKosync = false;
  bool grantsLibraryRead = false;

  bool usable() const { return !deviceId.empty() && token.size() >= 32; }
  // Every door this firmware opens with the token. A 200 that grants less is a
  // half-working reader, so it is refused with a next step instead.
  bool grantsEverythingTheReaderUses() const { return grantsSyncV2 && grantsKosync && grantsLibraryRead; }
  bool selfConsistent() const { return opdsUsername.empty() || opdsUsername == deviceId; }
};

enum class AccountTransition : uint8_t {
  FIRST_LOGIN,
  SAME_ACCOUNT,
  SWITCH_CLEAN,
  SWITCH_NEEDS_DECISION,
};

enum class SessionState : uint8_t {
  SIGNED_OUT,
  ACTIVE,
  RENEW_DUE,
  EXPIRED,
};

LoginError parseErrorCode(std::string_view code);
const char* errorName(LoginError error);
NextStep nextStepFor(LoginError error);
bool isRetryable(LoginError error);
// Used only when the body carries no usable error envelope.
LoginError errorForHttpStatus(int status);

// Streaming parsers: both walk the body once and copy out only the handful of
// fields above, so a 12 KiB response never lands in the heap as a DOM.
bool parseDeviceSession(const char* json, size_t length, DeviceSession& out);
bool parseErrorEnvelope(const char* json, size_t length, LoginError& out, std::string& requestId);

// Builds the login body. `password` is spent here and never retained.
std::string buildLoginRequest(std::string_view identifier, std::string_view password, std::string_view externalId,
                              std::string_view deviceName, std::string_view firmwareVersion);
// Stable across reboots and identical to the one the code-pairing flow uses, so
// both doors land on the same (account, platform, external_id) device row.
std::string deviceExternalId(uint64_t efuseMac);

// Case-folded comparison key. Two spellings of the same username must not look
// like two different accounts, or a switch would go undetected.
std::string accountFingerprint(std::string_view username);
AccountTransition classifyAccountTransition(std::string_view storedAccount, std::string_view incomingAccount,
                                            size_t pendingEvents);
SessionState sessionState(uint64_t nowUnixSeconds, uint64_t renewAfter, uint64_t expiresAt, bool hasToken);

}  // namespace toto
