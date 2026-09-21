#include "BorgesNetBootCodec.h"

#include <array>
#include <optional>

#include "BorgesSyncCore.h"

namespace borges {
namespace netboot {
namespace {

uint16_t be16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

// Advances `offset` past one encoded domain name. A compression pointer is
// treated as a TERMINATOR rather than followed: the answer names are never
// compared against the QNAME (see parseDnsResponseA), so there is nothing to
// gain from expanding them and a lot to lose (a full decompressor plus the
// pointer loops it invites). The 128-step guard and the bounds check on every
// step keep a hostile message from spinning or reading out of the buffer.
bool skipName(const uint8_t* message, size_t size, size_t& offset) {
  unsigned guard = 0;
  for (;;) {
    if (++guard > 128) return false;
    if (offset >= size) return false;
    const uint8_t length = message[offset];
    if (length == 0x00) {
      offset += 1;
      return true;
    }
    if ((length & 0xC0) == 0xC0) {
      if (offset + 1 >= size) return false;
      offset += 2;
      return true;
    }
    if ((length & 0xC0) != 0x00) return false;  // reserved label type
    if (offset + 1 + length > size) return false;
    offset += 1 + length;
  }
}

std::optional<unsigned> decimalAt(std::string_view value, size_t offset, size_t count) {
  unsigned result = 0;
  for (size_t index = 0; index < count; ++index) {
    const char digit = value[offset + index];
    if (digit < '0' || digit > '9') return std::nullopt;
    result = result * 10U + static_cast<unsigned>(digit - '0');
  }
  return result;
}

// Case-sensitive on purpose: IMF-fixdate fixes the spelling of the month.
std::optional<unsigned> monthFromAbbreviation(std::string_view value, size_t offset) {
  static constexpr std::array<const char*, 12> MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (unsigned index = 0; index < MONTHS.size(); ++index) {
    const char* name = MONTHS[index];
    if (value[offset] == name[0] && value[offset + 1] == name[1] && value[offset + 2] == name[2]) {
      return index + 1U;
    }
  }
  return std::nullopt;
}

}  // namespace

size_t buildDnsQueryTcp(const char* host, uint16_t id, uint8_t* out, size_t outSize) {
  if (host == nullptr || out == nullptr || outSize < 2 + 12 + 5) return 0;

  size_t hostLength = 0;
  while (host[hostLength] != '\0') {
    ++hostLength;
    if (hostLength > 254) return 0;  // longer than any legal name plus the root dot
  }
  // A single trailing dot is the explicit root label and carries no data.
  if (hostLength > 0 && host[hostLength - 1] == '.') --hostLength;
  if (hostLength == 0 || hostLength > 253) return 0;

  size_t offset = 2;  // out[0..1] is reserved for the TCP length prefix
  out[offset++] = static_cast<uint8_t>(id >> 8);
  out[offset++] = static_cast<uint8_t>(id & 0xFF);
  out[offset++] = 0x01;  // QR=0 Opcode=0 AA=0 TC=0 RD=1
  out[offset++] = 0x00;  // RA=0 Z=0 RCODE=0
  out[offset++] = 0x00;
  out[offset++] = 0x01;  // QDCOUNT = 1
  out[offset++] = 0x00;
  out[offset++] = 0x00;  // ANCOUNT
  out[offset++] = 0x00;
  out[offset++] = 0x00;  // NSCOUNT
  out[offset++] = 0x00;
  out[offset++] = 0x00;  // ARCOUNT

  const size_t qnameStart = offset;
  size_t labelStart = 0;
  for (size_t index = 0; index <= hostLength; ++index) {
    if (index < hostLength && host[index] != '.') {
      const unsigned char octet = static_cast<unsigned char>(host[index]);
      // Printable ASCII only, no spaces. No IDN/punycode: the host is ASCII by
      // construction (it comes from the baked-in base URL).
      if (octet < 0x21 || octet > 0x7E) return 0;
      continue;
    }
    const size_t labelLength = index - labelStart;
    if (labelLength == 0 || labelLength > 63) return 0;  // "..", leading dot, over-long label
    if (offset + 1 + labelLength > outSize) return 0;
    out[offset++] = static_cast<uint8_t>(labelLength);
    for (size_t position = 0; position < labelLength; ++position) {
      out[offset++] = static_cast<uint8_t>(host[labelStart + position]);
    }
    labelStart = index + 1;
  }

  if (offset + 1 > outSize) return 0;
  out[offset++] = 0x00;  // root label
  if (offset - qnameStart > 255) return 0;

  if (offset + 4 > outSize) return 0;
  out[offset++] = 0x00;
  out[offset++] = 0x01;  // QTYPE = A
  out[offset++] = 0x00;
  out[offset++] = 0x01;  // QCLASS = IN

  const size_t messageLength = offset - 2;
  out[0] = static_cast<uint8_t>(messageLength >> 8);
  out[1] = static_cast<uint8_t>(messageLength & 0xFF);
  return offset;
}

DnsParse parseDnsResponseA(const uint8_t* message, size_t size, uint16_t expectedId, uint8_t addr[4]) {
  if (message == nullptr || addr == nullptr || size < 12) return DnsParse::TOO_SHORT;
  if (be16(message) != expectedId) return DnsParse::ID_MISMATCH;
  if ((message[2] & 0x80) == 0) return DnsParse::NOT_RESPONSE;
  // The TC bit (message[2] & 0x02) is ignored: there is no truncation over TCP.
  if ((message[3] & 0x0F) != 0) return DnsParse::RCODE;

  const uint16_t questionCount = be16(message + 4);
  const uint16_t answerCount = be16(message + 6);
  if (questionCount > 4 || answerCount > 32) return DnsParse::MALFORMED;  // sanity bounds

  size_t offset = 12;
  for (uint16_t question = 0; question < questionCount; ++question) {
    if (!skipName(message, size, offset)) return DnsParse::MALFORMED;
    if (offset + 4 > size) return DnsParse::MALFORMED;
    offset += 4;  // QTYPE + QCLASS
  }

  // Deliberate: the answer names are NOT matched against the QNAME. With
  // compression plus CNAME chains that costs a full decompressor, and the
  // channel is unauthenticated plain DNS anyway -- the only real defence is
  // that the returned address has to serve a certificate that chains to the
  // pinned root and carries a SAN for the hostname, which the TLS handshake
  // checks right after. A poisoned answer breaks the handshake, it never
  // weakens it.
  for (uint16_t answer = 0; answer < answerCount; ++answer) {
    if (!skipName(message, size, offset)) return DnsParse::MALFORMED;
    if (offset + 10 > size) return DnsParse::MALFORMED;
    const uint16_t type = be16(message + offset);
    const uint16_t recordClass = be16(message + offset + 2);
    const uint16_t rdlength = be16(message + offset + 8);
    offset += 10;
    if (offset + rdlength > size) return DnsParse::MALFORMED;
    if (type == 1 && recordClass == 1 && rdlength == 4) {
      addr[0] = message[offset];
      addr[1] = message[offset + 1];
      addr[2] = message[offset + 2];
      addr[3] = message[offset + 3];
      return DnsParse::OK;
    }
    offset += rdlength;  // CNAME (type 5), AAAA (28) and friends are skipped
  }
  return DnsParse::NO_A_RECORD;
}

bool isPlausibleServerV4(const uint8_t addr[4]) {
  if (addr == nullptr) return false;
  if (addr[0] == 0) return false;                      // 0.0.0.0/8
  if (addr[0] == 127) return false;                    // loopback
  if (addr[0] == 169 && addr[1] == 254) return false;  // link-local
  if (addr[0] >= 224) return false;                    // multicast, reserved, 255.255.255.255
  return true;
}

uint64_t parseHttpDateEpoch(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);

  // IMF-fixdate is fixed width, 29 characters: "Www, DD Mon YYYY HH:MM:SS GMT".
  if (value.size() != 29) return 0;
  if (value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
      value[19] != ':' || value[22] != ':' || value[25] != ' ') {
    return 0;
  }
  if (value[26] != 'G' || value[27] != 'M' || value[28] != 'T') return 0;
  // The weekday (offsets 0..2) is ignored: it is redundant and some servers get
  // it wrong. Only its width is enforced, which the size check already did.

  const auto day = decimalAt(value, 5, 2);
  const auto month = monthFromAbbreviation(value, 8);
  const auto year = decimalAt(value, 12, 4);
  const auto hour = decimalAt(value, 17, 2);
  const auto minute = decimalAt(value, 20, 2);
  const auto second = decimalAt(value, 23, 2);
  if (!day || !month || !year || !hour || !minute || !second) return 0;
  if (*year < 1970 || *year > 9999 || *hour > 23 || *minute > 59 || *second > 60) return 0;

  static constexpr std::array<unsigned, 12> DAYS_PER_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leapYear = (*year % 4U == 0U && *year % 100U != 0U) || *year % 400U == 0U;
  const unsigned maxDay = *month == 2U && leapYear ? 29U : DAYS_PER_MONTH[*month - 1U];
  if (*day < 1U || *day > maxDay) return 0;
  // A leap second is clamped rather than rejected: it is a real value and a
  // one-second error is irrelevant for certificate validity.
  const unsigned clampedSecond = *second > 59U ? 59U : *second;

  const int64_t days = daysFromCivil(static_cast<int>(*year), *month, *day);
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400U + *hour * 3600U + *minute * 60U + clampedSecond;
}

}  // namespace netboot
}  // namespace borges
