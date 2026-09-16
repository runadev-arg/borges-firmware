#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace toto {

// Every request to the hub goes through here. One door keeps the two
// invariants the account contract leans on in a single readable place: https
// only, and the pinned root certificate. setInsecure() is never an option, so
// a token can never leave the device over an unverified connection.
struct HttpRequest {
  const char* path = nullptr;
  const std::string* body = nullptr;
  // nullptr for the unauthenticated calls (pairing, login).
  const char* bearerToken = nullptr;
  uint32_t timeoutMs = 15000;
  size_t maxResponseBytes = 12 * 1024;
  // Up-front room for the reply, so a long body does not walk the heap through
  // a chain of reallocations on a device with ~50 KB free.
  size_t reserveBytes = 2048;
  // The sync path already checks both before assembling its batch; the auth
  // paths ask this helper to do it.
  bool requireHeadroom = true;
  bool requireTrustedClock = true;
};

// A positive result is the HTTP status; these are the transport failures.
enum : int {
  HTTP_BEGIN_FAILED = -1,
  HTTP_INCOMPLETE = -2,
  HTTP_LOW_MEMORY = -3,
  HTTP_INSECURE_URL = -4,
  HTTP_CLOCK_ERROR = -5,
};

int postJson(const HttpRequest& request, std::string& response);

}  // namespace toto
