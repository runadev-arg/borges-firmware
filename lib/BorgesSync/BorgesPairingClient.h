#pragma once

#include <string>

namespace borges {

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

  static Result request(const char* deviceName = "Xteink X4 Borges");
  static Result pollAndClaim();
  static const char* resultName(Result result);
  static int lastHttpCode;
  // Transport-stage tag for the last failed request. Empty when the last
  // request got an HTTP response. The pairing UI appends it to the failure line
  // (and it is mirrored into the credential file) so a field device without
  // Serial access still reports WHY the network step died. Vocabulary:
  //   transport  "tcp", "dns", "tls:-188", "status_line", "http:-2"
  //   URL        "badurl:<prefix>", "badurl:host", "http_begin"
  //   resolution "resolve:none", "dns:ok", "dns:fail", "dnstcp:ok@1.1.1.1",
  //              "dnstcp:fail", "ip:cached", "ip:baked"
  //   clock      "clock ip:... gw:... d:... raw443:ok anchor:ok|sntp:ok|
  //              httpdate:ok|httpdate:fail(tcp|http|date|range|url)|
  //              httpsdate:ok|httpsdate:fail(status|date|range|<tls stage>)"
  //   retries    "try1", "try2"
  //   payload    "resp_too_big", "resp_incomplete", "bad_json", "bad_fields"
  static std::string lastErrorDetail;
};

}  // namespace borges
