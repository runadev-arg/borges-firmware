#pragma once

#include <cstdint>
#include <string>

#include "TotoDeviceLogin.h"

namespace toto {

// Signs the reader in with the account's own username and password, once, and
// keeps only the per-device credential the hub hands back.
class LoginClient {
 public:
  enum class Result : uint8_t {
    OK,
    REJECTED,  // the hub answered with a contract error; see Outcome::error
    LOW_MEMORY,
    CLOCK_ERROR,
    NETWORK_ERROR,
    INSECURE_URL,
    INVALID_RESPONSE,
  };

  struct Outcome {
    Result result = Result::INVALID_RESPONSE;
    LoginError error = LoginError::NONE;
    NextStep nextStep = NextStep::NONE;
    AccountTransition transition = AccountTransition::FIRST_LOGIN;
    int httpCode = 0;
    // Only filled on OK, and deliberately not persisted yet: a sign-in that
    // replaces another account waits for the reader to decide what happens to
    // the events still queued for the old one.
    DeviceSession session;
  };

  // Spends `password` on the request and wipes it before returning. Nothing
  // reaches disk here.
  static Outcome signIn(const std::string& identifier, std::string password);

  // Persists the session and rebuilds the catalogue/KOSync configuration.
  // `discardPreviousAccount` clears the outbox, inbox and cursor first.
  static bool commit(const DeviceSession& session, bool discardPreviousAccount);

  static void signOut();

  static const char* resultName(Result result);
};

}  // namespace toto
