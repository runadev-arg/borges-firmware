#include "TotoNetBoot.h"

#include <Arduino.h>
#include <SecureClient.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <strings.h>  // strcasecmp

#include "TotoCredentialStore.h"
#include "TotoNetBootCodec.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace netboot {
namespace {

// Per-boot memoization: the chain costs up to ~8 s (two TCP resolvers) and a
// pairing run asks for it twice (postJson and ensureTrustedClock).
std::string sMemoHost;
Candidates sMemo;
uint32_t sMemoAtMs = 0;
constexpr uint32_t MEMO_TTL_MS = 300000;  // 5 min

// A getaddrinfo against a dead DNS blocks for seconds (lwIP retries). Once it
// has failed in this boot there is no point paying for it again.
bool sGetaddrinfoDead = false;

constexpr char BAKED_HOST[] = "highlights.runadev.com";
// Cloudflare A records for the zone, verified 2026-07-26 (TTL 300). They are
// anycast and CAN change: this is the last resort, not the source of truth.
const IPAddress BAKED_IPS[] = {IPAddress(104, 21, 0, 166), IPAddress(172, 67, 128, 30)};

const IPAddress DNS_TCP_RESOLVERS[] = {IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8)};
constexpr uint32_t DNS_TCP_TIMEOUT_MS = 4000;

// Pointer form on purpose: building a std::string just to compare would heap
// allocate, and this runs on the resolution path.
bool sameHostCaseInsensitive(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) return false;
  return strcasecmp(left, right) == 0;
}

bool sameHostCaseInsensitive(const std::string& left, const char* right) {
  return sameHostCaseInsensitive(left.c_str(), right);
}

// Same shape as SecureHttpClient::readFixed: poll against a deadline instead of
// relying on the blocking semantics of Stream::readBytes, which a half-open
// socket (resolver accepts and never answers) would hang on.
bool readExact(WiFiClient& client, uint8_t* out, size_t count, uint32_t deadline) {
  size_t got = 0;
  while (got < count) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    const int chunk = client.read(out + got, count - got);
    if (chunk > 0) {
      got += static_cast<size_t>(chunk);
      continue;
    }
    if (!client.connected() && client.available() == 0) return false;
    delay(5);
  }
  return true;
}

bool queryDnsOverTcp(IPAddress resolver, const char* host, IPAddress& out) {
  uint8_t query[DNS_MAX_QUERY];  // ~274 B on the stack
  const uint16_t id = static_cast<uint16_t>(esp_random());
  const size_t queryLength = buildDnsQueryTcp(host, id, query, sizeof(query));
  if (queryLength == 0) return false;

  WiFiClient client;
  if (!client.connect(resolver, 53, DNS_TCP_TIMEOUT_MS)) return false;
  const uint32_t deadline = millis() + DNS_TCP_TIMEOUT_MS;
  bool ok = false;
  if (client.write(query, queryLength) == queryLength) {
    uint8_t prefix[2];
    if (readExact(client, prefix, 2, deadline)) {
      const size_t length = (static_cast<size_t>(prefix[0]) << 8) | static_cast<size_t>(prefix[1]);
      if (length >= 12 && length <= DNS_MAX_MESSAGE) {
        uint8_t message[DNS_MAX_MESSAGE];  // 512 B on the stack
        if (readExact(client, message, length, deadline)) {
          uint8_t addr[4];
          if (parseDnsResponseA(message, length, id, addr) == DnsParse::OK && isPlausibleServerV4(addr)) {
            out = IPAddress(addr[0], addr[1], addr[2], addr[3]);
            ok = true;
          }
        }
      }
      // length > DNS_MAX_MESSAGE: abandoned without draining; the stop() below
      // closes the socket anyway and the next resolver gets its turn.
    }
  }
  client.stop();
  return ok;
}

// One CRLF-terminated header line (CR stripped), with a per-line cap and an
// accumulated byte cap so a hostile peer cannot stream headers forever. A line
// longer than `maxLine` is TRUNCATED rather than fatal: Cloudflare emits long
// report-to/nel headers and aborting on one of them would hide the Date header
// behind it. The accumulated cap is what actually bounds the read.
bool readHeaderLine(WiFiClient& client, std::string& line, uint32_t deadline, size_t maxLine, size_t& consumed,
                    size_t maxTotal) {
  line.clear();
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    while (client.available() > 0) {
      const int ch = client.read();
      if (ch < 0) break;
      if (++consumed > maxTotal) return false;
      if (ch == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      if (line.size() < maxLine) line += static_cast<char>(ch);
    }
    if (!client.connected() && client.available() == 0) return false;
    delay(2);
  }
  return false;
}

}  // namespace

WifiFullPowerScope::WifiFullPowerScope() { esp_wifi_set_ps(WIFI_PS_NONE); }
WifiFullPowerScope::~WifiFullPowerScope() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }

std::string hostFromBaseUrl(const std::string& baseUrl) {
  const size_t schemeEnd = baseUrl.find("://");
  if (schemeEnd == std::string::npos) return {};
  std::string host = baseUrl.substr(schemeEnd + 3);
  const size_t pathStart = host.find_first_of("/?#");
  if (pathStart != std::string::npos) host.resize(pathStart);
  const size_t portStart = host.find(':');
  if (portStart != std::string::npos) host.resize(portStart);
  return host;
}

Candidates resolveServer(const char* host) {
  if (host == nullptr || *host == '\0') return {};
  if (sMemoHost == host && sMemo.count > 0 &&
      static_cast<int32_t>(millis() - sMemoAtMs) < static_cast<int32_t>(MEMO_TTL_MS)) {
    return sMemo;
  }

  Candidates out;
  auto push = [&out](IPAddress ip, Source source) {
    if (out.count >= MAX_CANDIDATES) return;
    for (uint8_t index = 0; index < out.count; ++index) {
      if (out.ip[index] == ip) return;  // dedup
    }
    const uint8_t octets[4] = {ip[0], ip[1], ip[2], ip[3]};
    if (!isPlausibleServerV4(octets)) return;
    out.ip[out.count] = ip;
    out.source[out.count] = source;
    ++out.count;
  };

  // (a) The DHCP-provided DNS.
  IPAddress viaDhcp;
  if (!sGetaddrinfoDead && freeink::SecureClient::resolveHostIPv4(host, viaDhcp)) {
    push(viaDhcp, Source::GETADDRINFO);
    out.detail = "dns:ok";
  } else {
    sGetaddrinfoDead = true;
    out.detail = "dns:fail";
  }

  // (b) DNS-over-TCP to public resolvers: needs no working DNS on the AP.
  if (out.count == 0) {
    for (const IPAddress& resolver : DNS_TCP_RESOLVERS) {
      IPAddress viaTcp;
      if (!queryDnsOverTcp(resolver, host, viaTcp)) continue;
      push(viaTcp, Source::DNS_TCP);
      out.detail = "dnstcp:ok@" + std::string(resolver.toString().c_str());
      break;
    }
    if (out.count == 0) out.detail += " dnstcp:fail";
  }

  // (c) Last known good IP.
  IPAddress cached;
  if (sameHostCaseInsensitive(TOTO_CREDENTIALS.getServerIpHost(), host) &&
      cached.fromString(TOTO_CREDENTIALS.getServerIp().c_str())) {
    if (out.count == 0) out.detail += " ip:cached";
    push(cached, Source::CACHED);
  }

  // (d) Baked-in: ONLY for the known production host.
  if (sameHostCaseInsensitive(BAKED_HOST, host)) {
    if (out.count == 0) out.detail += " ip:baked";
    for (const IPAddress& ip : BAKED_IPS) push(ip, Source::BAKED);
  }

  if (out.count > 0) {
    sMemoHost = host;
    sMemo = out;
    sMemoAtMs = millis();
  }
  return out;
}

void rememberServerIp(const char* host, IPAddress ip) {
  if (host == nullptr || *host == '\0') return;
  TOTO_CREDENTIALS.setServerIp(host, std::string(ip.toString().c_str()));
}

uint64_t httpDateEpoch(IPAddress ip, const char* host, std::string& detail) {
  constexpr uint32_t HTTP_DATE_CONNECT_MS = 4000;
  constexpr uint32_t HTTP_DATE_TIMEOUT_MS = 6000;
  constexpr size_t MAX_HEADER_BYTES = 2048;
  constexpr size_t MAX_LINE = 200;

  WiFiClient client;
  if (!client.connect(ip, 80, HTTP_DATE_CONNECT_MS)) {
    detail = "httpdate:fail(tcp)";
    return 0;
  }

  // HEAD: the answer is a 301 with headers and no body, so there is nothing to
  // drain. Connection: close keeps us from waiting on a keep-alive.
  std::string request = "HEAD / HTTP/1.1\r\nHost: ";
  request += host;
  request += "\r\nUser-Agent: CrossPoint-Borges/" CROSSPOINT_VERSION "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
  if (client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != request.size()) {
    client.stop();
    detail = "httpdate:fail(tcp)";
    return 0;
  }

  const uint32_t deadline = millis() + HTTP_DATE_TIMEOUT_MS;
  std::string line;
  size_t consumed = 0;
  bool sawStatusLine = false;
  uint64_t epoch = 0;
  while (readHeaderLine(client, line, deadline, MAX_LINE, consumed, MAX_HEADER_BYTES)) {
    if (!sawStatusLine) {
      // Only something that actually speaks HTTP gets to set the clock: a
      // captive portal answering garbage must not.
      if (line.compare(0, 7, "HTTP/1.") != 0) break;
      sawStatusLine = true;
      continue;
    }
    if (line.empty()) break;  // end of headers
    if (line.size() < 5) continue;
    if (strncasecmp(line.c_str(), "Date:", 5) != 0) continue;
    epoch = parseHttpDateEpoch(std::string_view(line).substr(5));
    break;
  }
  client.stop();

  if (!sawStatusLine) {
    detail = "httpdate:fail(http)";
    return 0;
  }
  if (epoch == 0) {
    detail = "httpdate:fail(date)";
    return 0;
  }
  if (epoch < static_cast<uint64_t>(MIN_TRUSTED_TIME) || epoch > static_cast<uint64_t>(MAX_TRUSTED_TIME)) {
    detail = "httpdate:fail(range)";
    return 0;
  }
  detail = "httpdate:ok";
  return epoch;
}

uint64_t httpsDateEpoch(IPAddress ip, const char* host, std::string& detail) {
  // Verification is intentionally OFF: this call exists because the network
  // killed every other clock source, and cert validation needs the very clock
  // being bootstrapped here (chicken and egg). The read value is bounded to
  // [MIN_TRUSTED_TIME, MAX_TRUSTED_TIME] below, which caps the damage of a
  // lying middlebox at what clear-text NTP already concedes. The instance is
  // local and never carries pairing traffic; the real exchange stays verified
  // against the pinned root.
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(8000);
  http.setReuse(false);
  http.setUserAgent(std::string("CrossPoint-Borges/") + CROSSPOINT_VERSION);
  if (!http.begin(std::string("https://") + host + "/")) {
    detail = "httpsdate:fail(url)";
    return 0;
  }
  http.setServerAddress(ip);
  // GET with a sink that refuses the first body chunk: the headers are all we
  // want, and aborting the body keeps both the wait and the heap at zero. HEAD
  // is avoided on purpose -- its Content-Length has no body behind it and the
  // body reader would wait for bytes that never come.
  const int status = http.sendRequest("GET", nullptr, 0, [](const uint8_t*, size_t) { return false; });
  const std::string date = http.getHeader("date");
  const std::string transport = http.lastErrorDetail();
  http.end();

  if (status <= 0) {
    detail = "httpsdate:fail(" + (transport.empty() ? "status" : transport) + ")";
    return 0;
  }
  const uint64_t epoch = parseHttpDateEpoch(date);
  if (epoch == 0) {
    detail = "httpsdate:fail(date)";
    return 0;
  }
  if (epoch < static_cast<uint64_t>(MIN_TRUSTED_TIME) || epoch > static_cast<uint64_t>(MAX_TRUSTED_TIME)) {
    detail = "httpsdate:fail(range)";
    return 0;
  }
  detail = "httpsdate:ok";
  return epoch;
}

}  // namespace netboot
}  // namespace toto
