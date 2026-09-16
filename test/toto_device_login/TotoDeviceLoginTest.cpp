#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lib/JsonParser/StreamingJsonParser.h"
#include "lib/TotoSync/TotoDeviceLogin.h"

namespace {

using toto::AccountTransition;
using toto::DeviceSession;
using toto::LoginError;
using toto::NextStep;
using toto::SessionState;

std::string readFixture(const std::string& name) {
  const std::string path = std::string(TOTO_LOGIN_FIXTURE_DIR) + "/" + name + ".json";
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "missing contract fixture: " << path;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// The fixtures are byte-identical copies of the ones the hub publishes, so they
// wrap the payload as {name, request, response:{status, body}}. Slice out the
// response body rather than reformatting the files: a contract drift upstream
// must show up here as a plain diff.
std::string responseBody(const std::string& fixture) {
  const size_t response = fixture.find("\"response\"");
  EXPECT_NE(response, std::string::npos);
  const size_t bodyKey = fixture.find("\"body\"", response);
  EXPECT_NE(bodyKey, std::string::npos);
  const size_t open = fixture.find('{', bodyKey);
  EXPECT_NE(open, std::string::npos);

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t index = open; index < fixture.size(); ++index) {
    const char c = fixture[index];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return fixture.substr(open, index - open + 1);
    }
  }
  ADD_FAILURE() << "unbalanced fixture body";
  return {};
}

int fixtureStatus(const std::string& fixture) {
  const size_t response = fixture.find("\"response\"");
  EXPECT_NE(response, std::string::npos);
  const size_t statusKey = fixture.find("\"status\"", response);
  EXPECT_NE(statusKey, std::string::npos);
  return std::atoi(fixture.c_str() + fixture.find(':', statusKey) + 1);
}

DeviceSession parseFixtureSession(const std::string& name) {
  const std::string body = responseBody(readFixture(name));
  DeviceSession session;
  EXPECT_TRUE(toto::parseDeviceSession(body.c_str(), body.size(), session)) << name;
  return session;
}

// Minimal object reader used to prove the request body we emit is valid JSON
// and that the values survive escaping unchanged.
struct FlatObject {
  std::vector<std::pair<std::string, std::string>> strings;
  std::vector<std::string> scopes;
  std::string key;
  int depth = 0;
  bool inScopes = false;
  bool valid = true;

  std::string string(const std::string& name) const {
    for (const auto& entry : strings) {
      if (entry.first == name) return entry.second;
    }
    return {};
  }
};

FlatObject readFlat(const std::string& json) {
  FlatObject state;
  const JsonCallbacks callbacks{
      &state,
      [](void* ctx, const char* key, size_t len) { static_cast<FlatObject*>(ctx)->key.assign(key, len); },
      [](void* ctx, const char* value, size_t len) {
        auto& s = *static_cast<FlatObject*>(ctx);
        if (s.inScopes) {
          s.scopes.emplace_back(value, len);
        } else if (s.depth == 1) {
          s.strings.emplace_back(s.key, std::string(value, len));
        }
        s.key.clear();
      },
      [](void* ctx, const char*, size_t) { static_cast<FlatObject*>(ctx)->key.clear(); },
      [](void* ctx, bool) { static_cast<FlatObject*>(ctx)->key.clear(); },
      [](void* ctx) { static_cast<FlatObject*>(ctx)->key.clear(); },
      [](void* ctx) {
        auto& s = *static_cast<FlatObject*>(ctx);
        ++s.depth;
        s.key.clear();
      },
      [](void* ctx) {
        auto& s = *static_cast<FlatObject*>(ctx);
        --s.depth;
        s.key.clear();
      },
      [](void* ctx) {
        auto& s = *static_cast<FlatObject*>(ctx);
        s.inScopes = s.depth == 1 && s.key == "scopes";
      },
      [](void* ctx) {
        auto& s = *static_cast<FlatObject*>(ctx);
        s.inScopes = false;
        s.key.clear();
      },
  };
  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  state.valid = !parser.hasError();
  return state;
}

TEST(TotoDeviceLogin, ParsesTheSuccessFixtureIntoASession) {
  const DeviceSession session = parseFixtureSession("success");

  EXPECT_EQ(session.accountUsername, "lectora");
  // credential.username, not device.id and not account.username: the three are
  // different fields and only this one authenticates OPDS and KOSync.
  EXPECT_EQ(session.deviceId, "11111111-1111-4111-8111-111111111111");
  EXPECT_EQ(session.token, "toto_XXXXXXXX_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
  EXPECT_EQ(session.tokenPrefix, "toto_XXXXXXXX");
  EXPECT_EQ(session.opdsUsername, "11111111-1111-4111-8111-111111111111");
  EXPECT_TRUE(session.usable());
  EXPECT_TRUE(session.selfConsistent());
}

TEST(TotoDeviceLogin, ReadsTheCredentialWindowFromTheSuccessFixture) {
  const DeviceSession session = parseFixtureSession("success");

  // 2027-03-14T09:00:00Z and 2027-02-28T09:00:00Z.
  EXPECT_EQ(session.expiresAt, 1805014800ULL);
  EXPECT_EQ(session.renewAfter, 1803805200ULL);
  EXPECT_LT(session.renewAfter, session.expiresAt);
}

TEST(TotoDeviceLogin, KeepsOnlyTheScopesThisReaderSpends) {
  const DeviceSession session = parseFixtureSession("success");

  EXPECT_TRUE(session.grantsSyncV2);
  EXPECT_TRUE(session.grantsKosync);
  EXPECT_TRUE(session.grantsLibraryRead);
  EXPECT_TRUE(session.grantsEverythingTheReaderUses());
}

TEST(TotoDeviceLogin, AGrantThatMissesADoorIsNotEnough) {
  const std::string body = R"({"credential":{"username":"dev","token":"toto_aaaaaaaa_bbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
                           R"("scopes":["device:self","sync:v2","kosync"]}})";
  DeviceSession session;

  ASSERT_TRUE(toto::parseDeviceSession(body.c_str(), body.size(), session));
  EXPECT_TRUE(session.usable());
  // library:read is missing, so the catalogue would silently not work.
  EXPECT_FALSE(session.grantsEverythingTheReaderUses());
}

TEST(TotoDeviceLogin, AResponseThatContradictsItselfIsNotConsistent) {
  const std::string body = R"({"credential":{"username":"dev-1","token":"toto_aaaaaaaa_bbbbbbbbbbbbbbbbbbbbbbbbbbb"},)"
                           R"("legacy":{"opds":{"username":"dev-2"}}})";
  DeviceSession session;

  ASSERT_TRUE(toto::parseDeviceSession(body.c_str(), body.size(), session));
  // The contract says legacy.opds.username is the device id; two answers in one
  // body means the reader would authenticate the catalogue as somebody else.
  EXPECT_FALSE(session.selfConsistent());
}

TEST(TotoDeviceLogin, EveryPublishedErrorFixtureResolvesToItsOwnNextStep) {
  struct Expectation {
    const char* fixture;
    int status;
    LoginError error;
    NextStep step;
  };
  const Expectation expectations[] = {
      {"invalid_credentials", 401, LoginError::INVALID_CREDENTIALS, NextStep::REENTER_CREDENTIALS},
      {"email_not_verified", 403, LoginError::EMAIL_NOT_VERIFIED, NextStep::VERIFY_EMAIL},
      {"device_revoked", 403, LoginError::DEVICE_REVOKED, NextStep::RELINK_FROM_WEB},
      {"tls_required", 403, LoginError::TLS_REQUIRED, NextStep::USE_HTTPS},
      {"scope_not_granted", 403, LoginError::SCOPE_NOT_GRANTED, NextStep::RELINK_FROM_WEB},
      {"device_limit_reached", 409, LoginError::DEVICE_LIMIT_REACHED, NextStep::FREE_A_DEVICE_SLOT},
      {"invalid_request", 422, LoginError::INVALID_REQUEST, NextStep::FIX_REQUEST},
      {"owner_not_accepted", 422, LoginError::OWNER_NOT_ACCEPTED, NextStep::FIX_REQUEST},
      {"rate_limited", 429, LoginError::RATE_LIMITED, NextStep::WAIT_AND_RETRY},
      {"login_unavailable", 503, LoginError::LOGIN_UNAVAILABLE, NextStep::WAIT_AND_RETRY},
  };

  for (const Expectation& expectation : expectations) {
    const std::string fixture = readFixture(expectation.fixture);
    const std::string body = responseBody(fixture);
    EXPECT_EQ(fixtureStatus(fixture), expectation.status) << expectation.fixture;

    LoginError error = LoginError::NONE;
    std::string requestId;
    ASSERT_TRUE(toto::parseErrorEnvelope(body.c_str(), body.size(), error, requestId)) << expectation.fixture;
    EXPECT_EQ(error, expectation.error) << expectation.fixture;
    EXPECT_EQ(toto::nextStepFor(error), expectation.step) << expectation.fixture;
    EXPECT_EQ(requestId, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa") << expectation.fixture;

    // An error body must never be mistaken for a session.
    DeviceSession session;
    EXPECT_FALSE(toto::parseDeviceSession(body.c_str(), body.size(), session)) << expectation.fixture;
  }
}

TEST(TotoDeviceLogin, OnlyRateLimitAndOutageAreWorthRetryingOnTheirOwn) {
  EXPECT_TRUE(toto::isRetryable(LoginError::RATE_LIMITED));
  EXPECT_TRUE(toto::isRetryable(LoginError::LOGIN_UNAVAILABLE));
  EXPECT_FALSE(toto::isRetryable(LoginError::INVALID_CREDENTIALS));
  EXPECT_FALSE(toto::isRetryable(LoginError::DEVICE_REVOKED));
  EXPECT_FALSE(toto::isRetryable(LoginError::DEVICE_LIMIT_REACHED));
}

TEST(TotoDeviceLogin, AnUnknownCodeWaitsInsteadOfPassing) {
  const std::string body = R"({"error":{"code":"teapot","message":"?"},"request_id":"r"})";
  LoginError error = LoginError::NONE;
  std::string requestId;

  ASSERT_TRUE(toto::parseErrorEnvelope(body.c_str(), body.size(), error, requestId));
  EXPECT_EQ(error, LoginError::UNKNOWN);
  EXPECT_EQ(toto::nextStepFor(error), NextStep::WAIT_AND_RETRY);
}

TEST(TotoDeviceLogin, AmbiguousStatusesDoNotGuessACause) {
  // 403 carries three different causes in the contract; only the body decides.
  EXPECT_EQ(toto::errorForHttpStatus(403), LoginError::UNKNOWN);
  EXPECT_EQ(toto::errorForHttpStatus(401), LoginError::INVALID_CREDENTIALS);
  EXPECT_EQ(toto::errorForHttpStatus(409), LoginError::DEVICE_LIMIT_REACHED);
  EXPECT_EQ(toto::errorForHttpStatus(422), LoginError::INVALID_REQUEST);
  EXPECT_EQ(toto::errorForHttpStatus(429), LoginError::RATE_LIMITED);
  EXPECT_EQ(toto::errorForHttpStatus(503), LoginError::LOGIN_UNAVAILABLE);
  EXPECT_EQ(toto::errorForHttpStatus(500), LoginError::UNKNOWN);
}

TEST(TotoDeviceLogin, RejectsAResponseWithoutAUsableCredential) {
  DeviceSession session;
  const std::string noToken = R"({"account":{"username":"a"},"credential":{"username":"dev"}})";
  EXPECT_FALSE(toto::parseDeviceSession(noToken.c_str(), noToken.size(), session));

  const std::string shortToken = R"({"credential":{"username":"dev","token":"toto_short"}})";
  EXPECT_FALSE(toto::parseDeviceSession(shortToken.c_str(), shortToken.size(), session));

  EXPECT_FALSE(toto::parseDeviceSession(nullptr, 0, session));
}

TEST(TotoDeviceLogin, ACutResponseIsNotASession) {
  const std::string body = responseBody(readFixture("success"));
  const size_t credential = body.find("\"credential\"");
  const size_t midToken = body.find("toto_XXXXXXXX_") + 20;
  ASSERT_NE(credential, std::string::npos);
  ASSERT_LT(midToken, body.size());

  // A connection that drops mid-body must not leave a half-session behind,
  // whether it dies before the credential or in the middle of the token.
  DeviceSession session;
  for (const size_t cut : {size_t{16}, credential, midToken}) {
    EXPECT_FALSE(toto::parseDeviceSession(body.c_str(), cut, session)) << "cut at " << cut;
    EXPECT_TRUE(session.token.empty()) << "cut at " << cut;
  }
}

TEST(TotoDeviceLogin, TheRequestNeverNamesTheAccount) {
  const std::string body = toto::buildLoginRequest("lectora", "s3cret", "x4-aabbccddeeff", "CrossPoint X4", "1.4.1");

  for (const char* forbidden : {"user_id", "userId", "account_id", "accountId", "owner_id", "ownerId"}) {
    EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
  }
}

TEST(TotoDeviceLogin, TheRequestCarriesTheFieldsTheContractRequires) {
  const std::string body = toto::buildLoginRequest("lectora", "s3cret", "x4-aabbccddeeff", "CrossPoint X4", "1.4.1");
  const FlatObject parsed = readFlat(body);

  ASSERT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.string("username"), "lectora");
  EXPECT_EQ(parsed.string("password"), "s3cret");
  EXPECT_EQ(parsed.string("platform"), toto::DEVICE_PLATFORM);
  EXPECT_EQ(parsed.string("external_id"), "x4-aabbccddeeff");
  EXPECT_EQ(parsed.string("device_name"), "CrossPoint X4");
  EXPECT_EQ(parsed.string("firmware_version"), "1.4.1");
  EXPECT_EQ(parsed.string("client_version"), "crosspoint-toto/1.4.1");
  EXPECT_NE(body.find("\"protocol_version\":2"), std::string::npos);
}

TEST(TotoDeviceLogin, TheRequestAsksOnlyForTheScopesTheReaderSpends) {
  const std::string body = toto::buildLoginRequest("lectora", "s3cret", "x4-aabbccddeeff", "CrossPoint X4", "1.4.1");
  const FlatObject parsed = readFlat(body);

  ASSERT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.scopes, (std::vector<std::string>{"device:self", "sync:v2", "library:read", "kosync"}));
  // Never requested: the reader neither installs plugins nor speaks sync v1.
  EXPECT_EQ(body.find("plugin:update"), std::string::npos);
  EXPECT_EQ(body.find("sync:legacy"), std::string::npos);
}

TEST(TotoDeviceLogin, AwkwardCredentialsSurviveTheRequestIntact) {
  const std::string password = "a\"b\\c\nd\te";
  const std::string body = toto::buildLoginRequest("lec\"tora", password, "x4-aabbccddeeff", "CrossPoint X4", "1.4.1");
  const FlatObject parsed = readFlat(body);

  ASSERT_TRUE(parsed.valid) << body;
  EXPECT_EQ(parsed.string("username"), "lec\"tora");
  EXPECT_EQ(parsed.string("password"), password);
}

TEST(TotoDeviceLogin, ControlCharactersAreEscapedRatherThanEmbedded) {
  const std::string body = toto::buildLoginRequest("u",
                                                   std::string("a\x01"
                                                               "b"),
                                                   "x4-1", "d", "1.0");

  EXPECT_NE(body.find("\\u0001"), std::string::npos);
  EXPECT_EQ(body.find('\x01'), std::string::npos);
  EXPECT_TRUE(readFlat(body).valid);
}

TEST(TotoDeviceLogin, TheExternalIdIsStableAndModelPrefixed) {
  EXPECT_EQ(toto::deviceExternalId(0xAABBCCDDEEFFULL), "x4-aabbccddeeff");
  EXPECT_EQ(toto::deviceExternalId(0xAABBCCDDEEFFULL), toto::deviceExternalId(0xAABBCCDDEEFFULL));
  // Only the 48 MAC bits take part, so a wider read of the same chip matches.
  EXPECT_EQ(toto::deviceExternalId(0xFFFF'AABBCCDDEEFFULL), "x4-aabbccddeeff");
  EXPECT_NE(toto::deviceExternalId(1), toto::deviceExternalId(2));
}

TEST(TotoDeviceLogin, TellsApartAFirstLoginFromASwitchAndAReturn) {
  EXPECT_EQ(toto::classifyAccountTransition("", "ana", 0), AccountTransition::FIRST_LOGIN);
  EXPECT_EQ(toto::classifyAccountTransition("", "ana", 7), AccountTransition::FIRST_LOGIN);
  EXPECT_EQ(toto::classifyAccountTransition("ana", "ana", 7), AccountTransition::SAME_ACCOUNT);
  EXPECT_EQ(toto::classifyAccountTransition("ana", "beto", 0), AccountTransition::SWITCH_CLEAN);
  EXPECT_EQ(toto::classifyAccountTransition("ana", "beto", 1), AccountTransition::SWITCH_NEEDS_DECISION);
}

TEST(TotoDeviceLogin, TwoSpellingsOfTheSameAccountAreNotASwitch) {
  EXPECT_EQ(toto::classifyAccountTransition("Ana", " ana ", 3), AccountTransition::SAME_ACCOUNT);
  EXPECT_EQ(toto::accountFingerprint("  Ana\t"), "ana");
  EXPECT_NE(toto::accountFingerprint("ana"), toto::accountFingerprint("ana2"));
}

TEST(TotoDeviceLogin, ReportsWhenTheCredentialIsDueOrSpent) {
  constexpr uint64_t RENEW = 2000;
  constexpr uint64_t EXPIRES = 3000;

  EXPECT_EQ(toto::sessionState(1000, RENEW, EXPIRES, false), SessionState::SIGNED_OUT);
  EXPECT_EQ(toto::sessionState(1000, RENEW, EXPIRES, true), SessionState::ACTIVE);
  EXPECT_EQ(toto::sessionState(RENEW, RENEW, EXPIRES, true), SessionState::RENEW_DUE);
  EXPECT_EQ(toto::sessionState(2500, RENEW, EXPIRES, true), SessionState::RENEW_DUE);
  EXPECT_EQ(toto::sessionState(EXPIRES, RENEW, EXPIRES, true), SessionState::EXPIRED);
  EXPECT_EQ(toto::sessionState(9999, RENEW, EXPIRES, true), SessionState::EXPIRED);
}

TEST(TotoDeviceLogin, AnUnknownClockNeverInventsAnExpiry) {
  EXPECT_EQ(toto::sessionState(0, 2000, 3000, true), SessionState::ACTIVE);
  // A response without a window keeps the session usable until the hub says no.
  EXPECT_EQ(toto::sessionState(9999, 0, 0, true), SessionState::ACTIVE);
}

}  // namespace
