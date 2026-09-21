#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Pure byte-level codec for the network bootstrap path: DNS-over-TCP framing
// and the HTTP Date header. Deliberately free of Arduino, WiFi and IPAddress so
// the whole thing is exercised by host gtest (test/borges_net_boot).

namespace borges {
namespace netboot {

// Largest DNS message we accept over TCP. An A answer for a name behind
// Cloudflare is ~100 bytes; 512 leaves room for an intermediate CNAME without
// risking the stack (heap is scarce here, so every buffer lives on the stack).
constexpr size_t DNS_MAX_MESSAGE = 512;
// 2-byte length prefix (RFC 1035 4.2.2) + header + QNAME + QTYPE/QCLASS.
constexpr size_t DNS_MAX_QUERY = 2 + 12 + 256 + 4;

enum class DnsParse : uint8_t {
  OK,
  TOO_SHORT,
  ID_MISMATCH,
  NOT_RESPONSE,
  RCODE,  // RCODE != 0 (NXDOMAIN, SERVFAIL, REFUSED)
  MALFORMED,
  NO_A_RECORD,  // well-formed response without any usable A/IN record
};

// Builds a standard DNS query (QTYPE=A, QCLASS=IN, RD=1) PRECEDED by the
// 2-byte length prefix that DNS-over-TCP requires. Returns the number of bytes
// written, or 0 when `host` is invalid or `out` is too small.
size_t buildDnsQueryTcp(const char* host, uint16_t id, uint8_t* out, size_t outSize);

// Parses a DNS message WITHOUT the length prefix. On success writes the four
// octets of the first A/IN record of the answer section into addr[].
DnsParse parseDnsResponseA(const uint8_t* message, size_t size, uint16_t expectedId, uint8_t addr[4]);

// Rejects addresses that cannot be a reachable web server:
// 0.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 224.0.0.0/4 and 255.255.255.255.
bool isPlausibleServerV4(const uint8_t addr[4]);

// RFC 7231 IMF-fixdate ("Sun, 26 Jul 2026 17:12:01 GMT") -> UTC epoch.
// Returns 0 when the value is not a well-formed IMF-fixdate. The obsolete
// formats (RFC 850, asctime) are rejected on purpose: no modern server emits
// them and the parser stays small. Note that epoch 0 doubles as the failure
// value; the caller's sanity window rejects it anyway.
uint64_t parseHttpDateEpoch(std::string_view value);

}  // namespace netboot
}  // namespace borges
