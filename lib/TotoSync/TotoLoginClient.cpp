#include "TotoLoginClient.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>

#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoHttp.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace {

constexpr uint32_t HTTP_TIMEOUT_MS = 20000;
// The success body runs to a few kilobytes; the cap keeps a hostile or broken
// server from filling the heap on a device that has ~50 KB to spare.
constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024;
constexpr char DEVICE_NAME[] = "CrossPoint X4";

// Overwrites the buffer before it is freed so the password does not linger in
// whatever heap block gets reused next.
void wipe(std::string& secret) {
  std::fill(secret.begin(), secret.end(), '\0');
  secret.clear();
  secret.shrink_to_fit();
}

LoginClient::Outcome transportOutcome(int status) {
  LoginClient::Outcome outcome;
  outcome.httpCode = status > 0 ? status : 0;
  switch (status) {
    case HTTP_LOW_MEMORY:
      outcome.result = LoginClient::Result::LOW_MEMORY;
      outcome.nextStep = NextStep::WAIT_AND_RETRY;
      return outcome;
    case HTTP_CLOCK_ERROR:
      outcome.result = LoginClient::Result::CLOCK_ERROR;
      outcome.nextStep = NextStep::RECONNECT;
      return outcome;
    case HTTP_INSECURE_URL:
      outcome.result = LoginClient::Result::INSECURE_URL;
      outcome.nextStep = NextStep::USE_HTTPS;
      return outcome;
    default:
      break;
  }
  outcome.result = LoginClient::Result::NETWORK_ERROR;
  outcome.nextStep = NextStep::RECONNECT;
  return outcome;
}

// The screen shows one instruction; the serial log keeps the distinction
// between "no network", "no trusted clock" and "no heap", which is what a
// report of "it just says check Wi-Fi" needs.
LoginClient::Outcome logged(LoginClient::Outcome outcome) {
  LOG_ERR("TOTO", "Account login failed: %s", LoginClient::resultName(outcome.result));
  return outcome;
}

}  // namespace

LoginClient::Outcome LoginClient::signIn(const std::string& identifier, std::string password) {
  std::string body =
      buildLoginRequest(identifier, password, deviceExternalId(ESP.getEfuseMac()), DEVICE_NAME, CROSSPOINT_VERSION);
  wipe(password);

  HttpRequest request;
  request.path = DEVICE_LOGIN_PATH;
  request.body = &body;
  request.timeoutMs = HTTP_TIMEOUT_MS;
  request.maxResponseBytes = MAX_RESPONSE_BYTES;

  std::string response;
  const int status = postJson(request, response);
  wipe(body);

  if (status <= 0) return logged(transportOutcome(status));

  Outcome outcome;
  outcome.httpCode = status;
  if (status != 200) {
    std::string requestId;
    if (!parseErrorEnvelope(response.c_str(), response.size(), outcome.error, requestId)) {
      outcome.error = errorForHttpStatus(status);
    }
    outcome.result = Result::REJECTED;
    outcome.nextStep = nextStepFor(outcome.error);
    // The request id is what support would ask for; the token never is, and
    // there is none on this path anyway.
    LOG_ERR("TOTO", "Account login refused: http=%d code=%s request=%s", status, errorName(outcome.error),
            requestId.c_str());
    return outcome;
  }

  if (!parseDeviceSession(response.c_str(), response.size(), outcome.session) || !outcome.session.selfConsistent()) {
    outcome.result = Result::INVALID_RESPONSE;
    outcome.nextStep = NextStep::WAIT_AND_RETRY;
    return logged(outcome);
  }

  // Defensive: the hub answers 403 scope_not_granted rather than trimming the
  // grant, so this only fires if that ever changes. Half the doors open is
  // worse than a clear "fix it on the web".
  if (!outcome.session.grantsEverythingTheReaderUses()) {
    outcome.result = Result::REJECTED;
    outcome.error = LoginError::SCOPE_NOT_GRANTED;
    outcome.nextStep = nextStepFor(outcome.error);
    outcome.session = {};
    return outcome;
  }

  outcome.result = Result::OK;
  outcome.transition = classifyAccountTransition(TOTO_CREDENTIALS.getAccountKey(), outcome.session.accountUsername,
                                                 TOTO_QUEUE.begin() ? TOTO_QUEUE.depth() : 0);
  LOG_INF("TOTO", "Account login accepted for device %s with credential %s", outcome.session.deviceId.c_str(),
          outcome.session.tokenPrefix.c_str());
  return outcome;
}

bool LoginClient::commit(const DeviceSession& session, bool discardPreviousAccount) {
  if (!session.usable()) return false;

  if (discardPreviousAccount) {
    // Order matters: the teardown reads the credential that is about to be
    // replaced to recognise which catalogue entry belongs to the old account.
    teardownCrossPointServices();
    if (!TOTO_QUEUE.purgeAccountState()) return false;
  }

  TOTO_CREDENTIALS.setSession(session);
  if (!TOTO_CREDENTIALS.saveToFile()) return false;
  return bootstrapCrossPointServices();
}

void LoginClient::signOut() {
  teardownCrossPointServices();
  TOTO_QUEUE.purgeAccountState();
  TOTO_CREDENTIALS.clearCredential();
  TOTO_CREDENTIALS.clearPairing();
  TOTO_CREDENTIALS.saveToFile();
}

const char* LoginClient::resultName(Result result) {
  switch (result) {
    case Result::OK:
      return "ok";
    case Result::REJECTED:
      return "rejected";
    case Result::LOW_MEMORY:
      return "low_memory";
    case Result::CLOCK_ERROR:
      return "clock_error";
    case Result::NETWORK_ERROR:
      return "network_error";
    case Result::INSECURE_URL:
      return "insecure_url";
    case Result::INVALID_RESPONSE:
      break;
  }
  return "invalid_response";
}

}  // namespace toto
