#pragma once

#include <IPAddress.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

// Socket-side half of the network bootstrap: turning a base URL into a list of
// dialable IPv4 candidates without depending on DNS-over-UDP, and reading the
// wall clock out of an HTTP Date header when SNTP is unreachable. The pure
// byte-level parts live in BorgesNetBootCodec (host-tested).

namespace borges {
namespace netboot {

constexpr size_t MAX_CANDIDATES = 4;
constexpr time_t MIN_TRUSTED_TIME = 1767225600;  // 2026-01-01T00:00:00Z
constexpr time_t MAX_TRUSTED_TIME = 2366841600;  // 2045-01-01T00:00:00Z

// Where each candidate came from, for the diagnostic string.
enum class Source : uint8_t { GETADDRINFO, DNS_TCP, CACHED, BAKED };

struct Candidates {
  IPAddress ip[MAX_CANDIDATES];
  Source source[MAX_CANDIDATES] = {};
  uint8_t count = 0;
  // Compact tag ready to append to an error detail:
  // "dns:ok" | "dnstcp:ok@1.1.1.1" | "dnstcp:fail" | "ip:cached" | "ip:baked"
  std::string detail;
};

// "https://borges.runadev.com/x" -> "borges.runadev.com" ("" if it does
// not parse). Any port suffix is stripped as well.
std::string hostFromBaseUrl(const std::string& baseUrl);

// Fallback chain: (a) lwip_getaddrinfo, (b) DNS-over-TCP to 1.1.1.1 and 8.8.8.8
// port 53, (c) the IP cached in the credential store, (d) baked-in IPs (only
// for the known production host). Deduplicates and memoizes per boot. Never
// returns count == 0 for the production host.
Candidates resolveServer(const char* host);

// Caches `ip` as the last known good result for `host` in the credential store.
// Does NOT call saveToFile(): the caller decides when to persist.
void rememberServerIp(const char* host, IPAddress ip);

// HTTP/1.1 HEAD to ip:80 with Host: host; parses the Date header and returns
// the UTC epoch, or 0. `detail` receives "httpdate:ok" or
// "httpdate:fail(<reason>)" with reason in {tcp, http, date, range}.
uint64_t httpDateEpoch(IPAddress ip, const char* host, std::string& detail);

// Same clock read over TLS on ip:443, for networks that intercept port 80 and
// block port 53 but let 443 through (seen in the field: carrier-filtered phone
// hotspots). Certificate verification is OFF for this request ONLY -- cert
// validation needs the very clock this call bootstraps -- and the value is
// sanity-bounded before anyone trusts it, so the exposure equals clear-text
// NTP. The pairing exchange never reuses this client and stays fully verified.
// `detail` receives "httpsdate:ok" or "httpsdate:fail(<reason>)" where reason
// is {status, date, range} or the transport stage ("tls:-188", "tcp", ...).
uint64_t httpsDateEpoch(IPAddress ip, const char* host, std::string& detail);

// Power save scope. Modem power save (WIFI_PS_MIN_MODEM, the default) makes
// some APs -- iPhone hotspots prominently -- drop UDP responses, so DNS and
// SNTP time out while the association looks healthy. Moved here from
// BorgesPairingClient.cpp so pairing and sync share one scope.
struct WifiFullPowerScope {
  WifiFullPowerScope();
  ~WifiFullPowerScope();
};

}  // namespace netboot
}  // namespace borges
