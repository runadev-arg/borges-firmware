#include "TotoDeviceLogin.h"

#include <StreamingJsonParser.h>

#include <array>
#include <cstdio>

#include "TotoSyncCore.h"

namespace toto {
namespace {

// Which top-level object of the response we are inside. `device` is parsed as
// NONE on purpose: its `id`/`username` fields would otherwise collide with the
// credential ones, and the reader only needs the credential.
enum class Section : uint8_t { NONE, ACCOUNT, CREDENTIAL, LEGACY };
enum class LegacySub : uint8_t { NONE, KOSYNC, OPDS };

Section sectionFor(std::string_view key) {
  if (key == "account") return Section::ACCOUNT;
  if (key == "credential") return Section::CREDENTIAL;
  if (key == "legacy") return Section::LEGACY;
  return Section::NONE;
}

LegacySub legacyFor(std::string_view key) {
  if (key == "kosync") return LegacySub::KOSYNC;
  if (key == "opds") return LegacySub::OPDS;
  return LegacySub::NONE;
}

// Object depth we track ourselves: 1 is the response root, 2 a top-level
// section, 3 a nested block such as legacy.kosync.
struct SessionContext {
  DeviceSession* out = nullptr;
  uint8_t depth = 0;
  Section section = Section::NONE;
  LegacySub legacySub = LegacySub::NONE;
  std::string key;
  bool inScopes = false;
};

void sessionKey(void* ctx, const char* key, size_t len) { static_cast<SessionContext*>(ctx)->key.assign(key, len); }

void sessionObjectStart(void* ctx) {
  auto& c = *static_cast<SessionContext*>(ctx);
  if (c.depth == 1) {
    c.section = sectionFor(c.key);
  } else if (c.depth == 2 && c.section == Section::LEGACY) {
    c.legacySub = legacyFor(c.key);
  }
  ++c.depth;
  c.key.clear();
}

void sessionObjectEnd(void* ctx) {
  auto& c = *static_cast<SessionContext*>(ctx);
  if (c.depth > 0) --c.depth;
  if (c.depth <= 1) c.section = Section::NONE;
  if (c.depth <= 2) c.legacySub = LegacySub::NONE;
  c.key.clear();
}

void sessionArrayStart(void* ctx) {
  auto& c = *static_cast<SessionContext*>(ctx);
  c.inScopes = c.depth == 2 && c.section == Section::CREDENTIAL && c.key == "scopes";
}

void sessionArrayEnd(void* ctx) {
  auto& c = *static_cast<SessionContext*>(ctx);
  c.inScopes = false;
  c.key.clear();
}

void sessionString(void* ctx, const char* value, size_t len) {
  auto& c = *static_cast<SessionContext*>(ctx);
  const std::string_view text(value, len);
  if (c.inScopes) {
    if (text == "sync:v2") c.out->grantsSyncV2 = true;
    if (text == "kosync") c.out->grantsKosync = true;
    if (text == "library:read") c.out->grantsLibraryRead = true;
    return;
  }
  if (c.section == Section::ACCOUNT && c.depth == 2) {
    if (c.key == "username") c.out->accountUsername.assign(text);
  } else if (c.section == Section::CREDENTIAL && c.depth == 2) {
    if (c.key == "token") {
      c.out->token.assign(text);
    } else if (c.key == "token_prefix") {
      c.out->tokenPrefix.assign(text);
    } else if (c.key == "username") {
      c.out->deviceId.assign(text);
    } else if (c.key == "expires_at") {
      c.out->expiresAt = parseRfc3339Utc(text).value_or(0);
    } else if (c.key == "renew_after") {
      c.out->renewAfter = parseRfc3339Utc(text).value_or(0);
    }
  } else if (c.section == Section::LEGACY && c.depth == 3) {
    if (c.legacySub == LegacySub::OPDS && c.key == "username") c.out->opdsUsername.assign(text);
  }
  c.key.clear();
}

void sessionNumber(void* ctx, const char*, size_t) { static_cast<SessionContext*>(ctx)->key.clear(); }

void sessionBool(void* ctx, bool) { static_cast<SessionContext*>(ctx)->key.clear(); }

void sessionNull(void* ctx) { static_cast<SessionContext*>(ctx)->key.clear(); }

struct ErrorContext {
  uint8_t depth = 0;
  bool inError = false;
  std::string key;
  std::string code;
  std::string requestId;
};

void errorKey(void* ctx, const char* key, size_t len) { static_cast<ErrorContext*>(ctx)->key.assign(key, len); }

void errorObjectStart(void* ctx) {
  auto& c = *static_cast<ErrorContext*>(ctx);
  if (c.depth == 1 && c.key == "error") c.inError = true;
  ++c.depth;
  c.key.clear();
}

void errorObjectEnd(void* ctx) {
  auto& c = *static_cast<ErrorContext*>(ctx);
  if (c.depth > 0) --c.depth;
  if (c.depth <= 1) c.inError = false;
  c.key.clear();
}

void errorString(void* ctx, const char* value, size_t len) {
  auto& c = *static_cast<ErrorContext*>(ctx);
  if (c.inError && c.depth == 2 && c.key == "code") {
    c.code.assign(value, len);
  } else if (!c.inError && c.depth == 1 && c.key == "request_id") {
    c.requestId.assign(value, len);
  }
  c.key.clear();
}

void errorNumber(void* ctx, const char*, size_t) { static_cast<ErrorContext*>(ctx)->key.clear(); }
void errorBool(void* ctx, bool) { static_cast<ErrorContext*>(ctx)->key.clear(); }
void errorNull(void* ctx) { static_cast<ErrorContext*>(ctx)->key.clear(); }
void errorArrayStart(void*) {}
void errorArrayEnd(void* ctx) { static_cast<ErrorContext*>(ctx)->key.clear(); }

void appendEscaped(std::string& target, std::string_view value) {
  for (const char raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (raw) {
      case '"':
        target += "\\\"";
        continue;
      case '\\':
        target += "\\\\";
        continue;
      case '\n':
        target += "\\n";
        continue;
      case '\r':
        target += "\\r";
        continue;
      case '\t':
        target += "\\t";
        continue;
      default:
        break;
    }
    if (byte < 0x20) {
      std::array<char, 7> escape{};
      std::snprintf(escape.data(), escape.size(), "\\u%04x", byte);
      target += escape.data();
      continue;
    }
    target.push_back(raw);
  }
}

void appendField(std::string& target, const char* name, std::string_view value) {
  target.push_back('"');
  target += name;
  target += "\":\"";
  appendEscaped(target, value);
  target += "\",";
}

char foldAscii(char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; }

}  // namespace

LoginError parseErrorCode(std::string_view code) {
  if (code.empty()) return LoginError::NONE;
  if (code == "invalid_credentials") return LoginError::INVALID_CREDENTIALS;
  if (code == "email_not_verified") return LoginError::EMAIL_NOT_VERIFIED;
  if (code == "device_revoked") return LoginError::DEVICE_REVOKED;
  if (code == "tls_required") return LoginError::TLS_REQUIRED;
  if (code == "scope_not_granted") return LoginError::SCOPE_NOT_GRANTED;
  if (code == "device_limit_reached") return LoginError::DEVICE_LIMIT_REACHED;
  if (code == "invalid_request") return LoginError::INVALID_REQUEST;
  if (code == "owner_not_accepted") return LoginError::OWNER_NOT_ACCEPTED;
  if (code == "rate_limited") return LoginError::RATE_LIMITED;
  if (code == "login_unavailable") return LoginError::LOGIN_UNAVAILABLE;
  return LoginError::UNKNOWN;
}

const char* errorName(LoginError error) {
  switch (error) {
    case LoginError::NONE:
      return "none";
    case LoginError::INVALID_CREDENTIALS:
      return "invalid_credentials";
    case LoginError::EMAIL_NOT_VERIFIED:
      return "email_not_verified";
    case LoginError::DEVICE_REVOKED:
      return "device_revoked";
    case LoginError::TLS_REQUIRED:
      return "tls_required";
    case LoginError::SCOPE_NOT_GRANTED:
      return "scope_not_granted";
    case LoginError::DEVICE_LIMIT_REACHED:
      return "device_limit_reached";
    case LoginError::INVALID_REQUEST:
      return "invalid_request";
    case LoginError::OWNER_NOT_ACCEPTED:
      return "owner_not_accepted";
    case LoginError::RATE_LIMITED:
      return "rate_limited";
    case LoginError::LOGIN_UNAVAILABLE:
      return "login_unavailable";
    case LoginError::UNKNOWN:
      break;
  }
  return "unknown";
}

NextStep nextStepFor(LoginError error) {
  switch (error) {
    case LoginError::NONE:
      return NextStep::NONE;
    case LoginError::INVALID_CREDENTIALS:
      return NextStep::REENTER_CREDENTIALS;
    case LoginError::EMAIL_NOT_VERIFIED:
      return NextStep::VERIFY_EMAIL;
    case LoginError::DEVICE_REVOKED:
    case LoginError::SCOPE_NOT_GRANTED:
      return NextStep::RELINK_FROM_WEB;
    case LoginError::TLS_REQUIRED:
      return NextStep::USE_HTTPS;
    case LoginError::DEVICE_LIMIT_REACHED:
      return NextStep::FREE_A_DEVICE_SLOT;
    case LoginError::INVALID_REQUEST:
    case LoginError::OWNER_NOT_ACCEPTED:
      return NextStep::FIX_REQUEST;
    case LoginError::RATE_LIMITED:
    case LoginError::LOGIN_UNAVAILABLE:
    case LoginError::UNKNOWN:
      break;
  }
  return NextStep::WAIT_AND_RETRY;
}

bool isRetryable(LoginError error) {
  return error == LoginError::RATE_LIMITED || error == LoginError::LOGIN_UNAVAILABLE;
}

LoginError errorForHttpStatus(int status) {
  switch (status) {
    case 401:
      return LoginError::INVALID_CREDENTIALS;
    case 409:
      return LoginError::DEVICE_LIMIT_REACHED;
    case 422:
      return LoginError::INVALID_REQUEST;
    case 429:
      return LoginError::RATE_LIMITED;
    case 503:
      return LoginError::LOGIN_UNAVAILABLE;
    default:
      // 403 is deliberately absent: the contract puts three different causes
      // behind it, so guessing one would send the reader down the wrong path.
      return LoginError::UNKNOWN;
  }
}

bool parseDeviceSession(const char* json, size_t length, DeviceSession& out) {
  if (json == nullptr || length == 0) return false;
  out = DeviceSession{};
  SessionContext context;
  context.out = &out;
  const JsonCallbacks callbacks{
      &context,    sessionKey,         sessionString,    sessionNumber,     sessionBool,
      sessionNull, sessionObjectStart, sessionObjectEnd, sessionArrayStart, sessionArrayEnd,
  };
  StreamingJsonParser parser(callbacks);
  parser.feed(json, length);
  if (parser.hasError()) return false;
  return out.usable();
}

bool parseErrorEnvelope(const char* json, size_t length, LoginError& out, std::string& requestId) {
  out = LoginError::NONE;
  requestId.clear();
  if (json == nullptr || length == 0) return false;
  ErrorContext context;
  const JsonCallbacks callbacks{
      &context,  errorKey,         errorString,    errorNumber,     errorBool,
      errorNull, errorObjectStart, errorObjectEnd, errorArrayStart, errorArrayEnd,
  };
  StreamingJsonParser parser(callbacks);
  parser.feed(json, length);
  if (parser.hasError() || context.code.empty()) return false;
  out = parseErrorCode(context.code);
  requestId = context.requestId;
  return true;
}

std::string buildLoginRequest(std::string_view identifier, std::string_view password, std::string_view externalId,
                              std::string_view deviceName, std::string_view firmwareVersion) {
  // No user_id / account_id in this body, by contract: the reader never names
  // its account, and sending one is a 422 instead of a silent mis-binding.
  std::string body;
  body.reserve(identifier.size() + password.size() + 320);
  body.push_back('{');
  appendField(body, "username", identifier);
  appendField(body, "password", password);
  appendField(body, "platform", DEVICE_PLATFORM);
  appendField(body, "external_id", externalId);
  appendField(body, "device_name", deviceName);
  appendField(body, "firmware_version", firmwareVersion);
  appendField(body, "client_version", std::string("crosspoint-toto/").append(firmwareVersion));
  body += "\"protocol_version\":2,";
  // Only the scopes this firmware actually spends, so the hub can refuse the
  // rest without the reader ever holding them.
  body += "\"scopes\":[\"device:self\",\"sync:v2\",\"library:read\",\"kosync\"],";
  body += "\"capabilities\":{\"annotations\":true,\"progress\":true,\"opds\":true,\"kosync\":true}}";
  return body;
}

std::string deviceExternalId(uint64_t efuseMac) {
  std::array<char, 32> value{};
  std::snprintf(value.data(), value.size(), "x4-%012llx",
                static_cast<unsigned long long>(efuseMac & 0xFFFFFFFFFFFFULL));
  return value.data();
}

std::string accountFingerprint(std::string_view username) {
  size_t begin = 0;
  size_t end = username.size();
  while (begin < end && (username[begin] == ' ' || username[begin] == '\t')) ++begin;
  while (end > begin && (username[end - 1] == ' ' || username[end - 1] == '\t')) --end;
  std::string folded;
  folded.reserve(end - begin);
  for (size_t index = begin; index < end; ++index) folded.push_back(foldAscii(username[index]));
  return folded;
}

AccountTransition classifyAccountTransition(std::string_view storedAccount, std::string_view incomingAccount,
                                            size_t pendingEvents) {
  const std::string stored = accountFingerprint(storedAccount);
  if (stored.empty()) return AccountTransition::FIRST_LOGIN;
  if (stored == accountFingerprint(incomingAccount)) return AccountTransition::SAME_ACCOUNT;
  return pendingEvents == 0 ? AccountTransition::SWITCH_CLEAN : AccountTransition::SWITCH_NEEDS_DECISION;
}

SessionState sessionState(uint64_t nowUnixSeconds, uint64_t renewAfter, uint64_t expiresAt, bool hasToken) {
  if (!hasToken) return SessionState::SIGNED_OUT;
  // An unknown clock must not invent an expiry: the reader keeps working and
  // learns the truth from the next 401.
  if (nowUnixSeconds == 0) return SessionState::ACTIVE;
  if (expiresAt != 0 && nowUnixSeconds >= expiresAt) return SessionState::EXPIRED;
  if (renewAfter != 0 && nowUnixSeconds >= renewAfter) return SessionState::RENEW_DUE;
  return SessionState::ACTIVE;
}

}  // namespace toto
