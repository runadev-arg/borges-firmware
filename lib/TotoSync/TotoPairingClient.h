#pragma once

namespace toto {

class PairingClient {
 public:
  enum class Result {
    OK,
    PENDING,
    PAIRED,
    REJECTED,
    EXPIRED,
    NO_REQUEST,
    LOW_MEMORY,
    CLOCK_ERROR,
    NETWORK_ERROR,
    SERVER_ERROR,
    INVALID_RESPONSE,
    PERSISTENCE_ERROR,
  };

  static Result request(const char* deviceName = "Xteink X4 Toto");
  static Result pollAndClaim();
  static const char* resultName(Result result);
  static int lastHttpCode;
};

}  // namespace toto
