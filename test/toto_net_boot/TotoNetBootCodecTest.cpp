#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "TotoNetBootCodec.h"
#include "TotoSyncCore.h"

namespace {

using toto::netboot::DnsParse;

// Builds a raw DNS message (no TCP length prefix) byte by byte.
class MessageBuilder {
 public:
  MessageBuilder& u8(uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }
  MessageBuilder& u16(uint16_t value) {
    bytes_.push_back(static_cast<uint8_t>(value >> 8));
    bytes_.push_back(static_cast<uint8_t>(value & 0xFF));
    return *this;
  }
  MessageBuilder& u32(uint32_t value) {
    u16(static_cast<uint16_t>(value >> 16));
    u16(static_cast<uint16_t>(value & 0xFFFF));
    return *this;
  }
  MessageBuilder& raw(std::initializer_list<uint8_t> values) {
    bytes_.insert(bytes_.end(), values.begin(), values.end());
    return *this;
  }
  // Header with QR=1, RD=1, RA=1 and the given RCODE / counts.
  MessageBuilder& header(uint16_t id, uint16_t questions, uint16_t answers, uint8_t rcode = 0, bool response = true) {
    u16(id);
    u8(response ? 0x81 : 0x01);
    u8(static_cast<uint8_t>(0x80 | (rcode & 0x0F)));
    u16(questions);
    u16(answers);
    u16(0);
    u16(0);
    return *this;
  }
  // "a.bc" question, so the compression pointer 0xC00C always resolves to it.
  MessageBuilder& questionABc() { return raw({0x01, 'a', 0x02, 'b', 'c', 0x00}).u16(1).u16(1); }
  MessageBuilder& record(uint16_t type, uint16_t recordClass, std::initializer_list<uint8_t> rdata) {
    raw({0xC0, 0x0C});  // compressed name pointing at the question
    u16(type);
    u16(recordClass);
    u32(300);
    u16(static_cast<uint16_t>(rdata.size()));
    return raw(rdata);
  }

  const uint8_t* data() const { return bytes_.data(); }
  size_t size() const { return bytes_.size(); }
  std::vector<uint8_t>& bytes() { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

constexpr uint16_t ID = 0x1234;

}  // namespace

// --- buildDnsQueryTcp -------------------------------------------------------

// Case 1: exact byte vector from the plan.
TEST(TotoNetBootCodec, BuildsShortQueryByteForByte) {
  std::array<uint8_t, toto::netboot::DNS_MAX_QUERY> out{};
  const size_t written = toto::netboot::buildDnsQueryTcp("a.bc", ID, out.data(), out.size());
  ASSERT_EQ(written, 24U);
  const std::array<uint8_t, 24> expected = {0x00, 0x16, 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x01, 0x61, 0x02, 0x62, 0x63, 0x00, 0x00, 0x01, 0x00, 0x01};
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(out[index], expected[index]) << "byte " << index;
  }
}

// Case 2: the production host.
TEST(TotoNetBootCodec, BuildsProductionHostQuery) {
  std::array<uint8_t, toto::netboot::DNS_MAX_QUERY> out{};
  const size_t written = toto::netboot::buildDnsQueryTcp("highlights.runadev.com", ID, out.data(), out.size());
  ASSERT_EQ(written, 42U);
  EXPECT_EQ(out[0], 0x00);
  EXPECT_EQ(out[1], 0x28);  // 40-byte message

  const std::vector<uint8_t> expectedQname = {0x0A, 'h', 'i', 'g', 'h', 'l', 'i', 'g',  'h', 't', 's', 0x07,
                                              'r',  'u', 'n', 'a', 'd', 'e', 'v', 0x03, 'c', 'o', 'm', 0x00};
  for (size_t index = 0; index < expectedQname.size(); ++index) {
    EXPECT_EQ(out[14 + index], expectedQname[index]) << "qname byte " << index;
  }
  EXPECT_EQ(out[38], 0x00);
  EXPECT_EQ(out[39], 0x01);  // QTYPE = A
  EXPECT_EQ(out[40], 0x00);
  EXPECT_EQ(out[41], 0x01);  // QCLASS = IN
}

// Case 3: every rejected input shape.
TEST(TotoNetBootCodec, RejectsMalformedHostsAndSmallBuffers) {
  std::array<uint8_t, toto::netboot::DNS_MAX_QUERY> out{};
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp("", ID, out.data(), out.size()), 0U);
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp(nullptr, ID, out.data(), out.size()), 0U);
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp("a..b", ID, out.data(), out.size()), 0U);
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp(".a", ID, out.data(), out.size()), 0U);
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp("a b.com", ID, out.data(), out.size()), 0U);

  const std::string longLabel(64, 'a');
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp((longLabel + ".com").c_str(), ID, out.data(), out.size()), 0U);
  const std::string longLabelOk(63, 'a');
  EXPECT_GT(toto::netboot::buildDnsQueryTcp((longLabelOk + ".com").c_str(), ID, out.data(), out.size()), 0U);

  std::string longHost;
  while (longHost.size() < 300) longHost += "abcdefgh.";
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp(longHost.c_str(), ID, out.data(), out.size()), 0U);

  EXPECT_EQ(toto::netboot::buildDnsQueryTcp("a.bc", ID, out.data(), 20), 0U);  // needs 24
  EXPECT_EQ(toto::netboot::buildDnsQueryTcp("a.bc", ID, out.data(), 8), 0U);
}

// Case 4: a trailing dot is the root label and changes nothing.
TEST(TotoNetBootCodec, AcceptsTrailingRootDot) {
  std::array<uint8_t, toto::netboot::DNS_MAX_QUERY> withDot{};
  std::array<uint8_t, toto::netboot::DNS_MAX_QUERY> withoutDot{};
  const size_t a = toto::netboot::buildDnsQueryTcp("a.bc.", ID, withDot.data(), withDot.size());
  const size_t b = toto::netboot::buildDnsQueryTcp("a.bc", ID, withoutDot.data(), withoutDot.size());
  ASSERT_EQ(a, 24U);
  ASSERT_EQ(b, 24U);
  EXPECT_EQ(std::vector<uint8_t>(withDot.begin(), withDot.begin() + 24),
            std::vector<uint8_t>(withoutDot.begin(), withoutDot.begin() + 24));
}

// --- parseDnsResponseA ------------------------------------------------------

// Case 5: a single direct A record.
TEST(TotoNetBootCodec, ParsesSingleARecord) {
  MessageBuilder message;
  message.header(ID, 1, 1).questionABc().record(1, 1, {104, 21, 0, 166});

  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::OK);
  EXPECT_EQ(addr[0], 104);
  EXPECT_EQ(addr[1], 21);
  EXPECT_EQ(addr[2], 0);
  EXPECT_EQ(addr[3], 166);
}

// Case 6: CNAME first, A second, both names compressed.
TEST(TotoNetBootCodec, SkipsCnameAndReturnsA) {
  MessageBuilder message;
  message.header(ID, 1, 2).questionABc().record(5, 1, {0xC0, 0x0C}).record(1, 1, {104, 21, 0, 166});

  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::OK);
  EXPECT_EQ(addr[0], 104);
  EXPECT_EQ(addr[3], 166);
}

// Case 7.
TEST(TotoNetBootCodec, RejectsIdMismatch) {
  MessageBuilder message;
  message.header(ID, 1, 1).questionABc().record(1, 1, {104, 21, 0, 166});
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), 0x4321, addr), DnsParse::ID_MISMATCH);
}

// Case 8.
TEST(TotoNetBootCodec, RejectsQueryFlag) {
  MessageBuilder message;
  message.header(ID, 1, 1, 0, /*response=*/false).questionABc().record(1, 1, {104, 21, 0, 166});
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::NOT_RESPONSE);
}

// Case 9.
TEST(TotoNetBootCodec, RejectsNonZeroRcode) {
  MessageBuilder message;
  message.header(ID, 1, 0, /*rcode=*/3).questionABc();
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::RCODE);
}

TEST(TotoNetBootCodec, RejectsTooShortMessage) {
  const std::array<uint8_t, 8> tooShort = {0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01};
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(tooShort.data(), tooShort.size(), ID, addr), DnsParse::TOO_SHORT);
}

// Case 10: ANCOUNT=1 but the message stops halfway through the RDATA.
TEST(TotoNetBootCodec, RejectsTruncatedRdata) {
  MessageBuilder message;
  message.header(ID, 1, 1).questionABc().record(1, 1, {104, 21, 0, 166});
  message.bytes().resize(message.bytes().size() - 2);  // cut into the RDATA

  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::MALFORMED);
}

// Case 11: pointer loops and label floods terminate instead of hanging.
TEST(TotoNetBootCodec, SurvivesPointerLoopAndLabelFlood) {
  MessageBuilder selfPointer;
  selfPointer.header(ID, 0, 1).raw({0xC0, 0x0C, 0x00, 0x01, 0x00});  // name points at itself, record truncated
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(selfPointer.data(), selfPointer.size(), ID, addr), DnsParse::MALFORMED);

  MessageBuilder labelFlood;
  labelFlood.header(ID, 0, 1);
  for (int label = 0; label < 200; ++label) labelFlood.raw({0x01, 'a'});  // never terminated
  EXPECT_EQ(toto::netboot::parseDnsResponseA(labelFlood.data(), labelFlood.size(), ID, addr), DnsParse::MALFORMED);
}

// Case 12.
TEST(TotoNetBootCodec, ReportsCnameOnlyAnswer) {
  MessageBuilder message;
  message.header(ID, 1, 1).questionABc().record(5, 1, {0xC0, 0x0C});
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::NO_A_RECORD);
}

// Case 13: AAAA only.
TEST(TotoNetBootCodec, IgnoresAaaaRecords) {
  MessageBuilder message;
  message.header(ID, 1, 1).questionABc().record(
      28, 1, {0x26, 0x06, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x68, 0x15, 0x00, 0xA6});
  uint8_t addr[4] = {};
  EXPECT_EQ(toto::netboot::parseDnsResponseA(message.data(), message.size(), ID, addr), DnsParse::NO_A_RECORD);
}

// --- parseHttpDateEpoch -----------------------------------------------------

// Case 14.
TEST(TotoNetBootCodec, ParsesImfFixdate) {
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 17:12:01 GMT"), 1785085921ULL);
}

// Case 15: epoch 0 doubles as the failure value, so the first representable
// value that can be distinguished is one day later. That ambiguity is harmless:
// the caller's sanity window rejects anything before 2026 anyway.
TEST(TotoNetBootCodec, ParsesEarlyEpochValues) {
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Thu, 01 Jan 1970 00:00:00 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Fri, 02 Jan 1970 00:00:00 GMT"), 86400ULL);
}

// Case 16: every rejected shape.
TEST(TotoNetBootCodec, RejectsNonImfFixdate) {
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 17:12:01"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sunday, 26-Jul-26 17:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun Jul 26 17:12:01 2026"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Xxx 2026 17:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 32 Jul 2026 17:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 29 Feb 2026 17:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 24:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 17:60:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 17:12:01 UTC"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 1969 17:12:01 GMT"), 0ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch(""), 0ULL);
}

// Case 17: exactly how the value arrives after stripping "Date:".
TEST(TotoNetBootCodec, TrimsSurroundingWhitespace) {
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch(" Sun, 26 Jul 2026 17:12:01 GMT "), 1785085921ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("\tSun, 26 Jul 2026 17:12:01 GMT\t"), 1785085921ULL);
}

// Case 18: a real leap day, plus the leap-second clamp.
TEST(TotoNetBootCodec, AcceptsLeapDayAndClampsLeapSecond) {
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Mon, 29 Feb 2024 00:00:00 GMT"), 1709164800ULL);
  EXPECT_EQ(toto::netboot::parseHttpDateEpoch("Sun, 26 Jul 2026 17:12:60 GMT"), 1785085979ULL);
}

// --- isPlausibleServerV4 ----------------------------------------------------

// Case 19.
TEST(TotoNetBootCodec, FiltersUnroutableAddresses) {
  auto plausible = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t addr[4] = {a, b, c, d};
    return toto::netboot::isPlausibleServerV4(addr);
  };
  EXPECT_FALSE(plausible(0, 0, 0, 0));
  EXPECT_FALSE(plausible(0, 1, 2, 3));
  EXPECT_FALSE(plausible(127, 0, 0, 1));
  EXPECT_FALSE(plausible(169, 254, 1, 1));
  EXPECT_FALSE(plausible(224, 0, 0, 1));
  EXPECT_FALSE(plausible(255, 255, 255, 255));
  EXPECT_TRUE(plausible(104, 21, 0, 166));
  EXPECT_TRUE(plausible(172, 67, 128, 30));
  EXPECT_TRUE(plausible(192, 168, 1, 10));
}

// --- daysFromCivil ----------------------------------------------------------

// Case 20: the factored-out helper must keep behaving exactly as the copy that
// used to live inside parseRfc3339Utc (whose own tests stay untouched).
TEST(TotoNetBootCodec, DaysFromCivilMatchesKnownEpochs) {
  EXPECT_EQ(toto::daysFromCivil(1970, 1, 1), 0);
  EXPECT_EQ(toto::daysFromCivil(1970, 1, 2), 1);
  EXPECT_EQ(toto::daysFromCivil(2000, 3, 1), 11017);
  EXPECT_EQ(toto::daysFromCivil(2024, 2, 29), 19782);
  EXPECT_EQ(toto::daysFromCivil(2026, 1, 1), 20454);
  EXPECT_EQ(toto::daysFromCivil(2026, 7, 26), 20660);
  EXPECT_EQ(toto::daysFromCivil(1969, 12, 31), -1);

  ASSERT_TRUE(toto::parseRfc3339Utc("2026-07-26T17:12:01Z").has_value());
  EXPECT_EQ(*toto::parseRfc3339Utc("2026-07-26T17:12:01Z"), 1785085921ULL);
}
