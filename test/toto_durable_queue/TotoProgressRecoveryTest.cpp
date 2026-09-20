#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include "TotoProgressRecovery.h"
namespace {
constexpr const char* hash = "463bae244c31108170d30c5fd733918d";
JsonDocument response() {
  JsonDocument d;
  d["protocol_version"] = 2;
  d["positions"].to<JsonArray>();
  return d;
}
void position(JsonDocument& d, const char* id, bool current, const char* revision, double percentage) {
  auto p = d["positions"].as<JsonArray>().add<JsonObject>();
  p["book_identifier"]["kind"] = "koreader_partial_md5";
  p["book_identifier"]["value"] = hash;
  p["device_id"] = id;
  p["device_name"] = id;
  p["is_current_device"] = current;
  p["revision"] = revision;
  p["percentage"] = percentage;
  p["xpointer"] = "/body/DocFragment[19]/body/h2/text().0";
}
JsonDocument event(const toto::RecoverySelection& s) {
  JsonDocument d;
  EXPECT_FALSE(s.eventJson.empty());
  EXPECT_FALSE(deserializeJson(d, s.eventJson));
  return d;
}
TEST(TotoProgressRecovery, LatestOtherIgnoresNewerOwnZeroAfterReconnect) {
  auto d = response();
  position(d, "x4", true, "85", 0);
  position(d, "kindle", false, "70", 30.43);
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  ASSERT_TRUE(s.valid);
  auto e = event(s);
  EXPECT_NEAR(e["payload"]["percentage"].as<double>(), 30.43, 0.00001);
  EXPECT_STREQ(e["origin_device"]["id"], "kindle");
  EXPECT_TRUE(e["server_sequence"].isNull());
}
TEST(TotoProgressRecovery, LatestAnyIncludesCurrentDevice) {
  auto d = response();
  position(d, "x4", true, "85", 0);
  position(d, "kindle", false, "70", 30.43);
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, false);
  ASSERT_TRUE(s.valid);
  auto e = event(s);
  EXPECT_STREQ(e["origin_device"]["id"], "x4");
}
TEST(TotoProgressRecovery, LegacyKindlePositionSurvivesWithoutInventingRevision) {
  auto d = response();
  position(d, "kindle", false, nullptr, 30.43);
  auto p = d["positions"][0];
  p["source_protocol"] = "legacy";
  p["recovery_id"] = "legacy:42";
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  ASSERT_TRUE(s.valid);
  auto e = event(s);
  EXPECT_TRUE(e["source_revision"].isNull());
  EXPECT_STREQ(e["source_recovery_id"], "legacy:42");
}
TEST(TotoProgressRecovery, UsesServerChronologyAcrossLegacyAndV2Counters) {
  auto d = response();
  position(d, "kindle", false, nullptr, 31);
  d["positions"][0]["source_protocol"] = "legacy";
  d["positions"][0]["recovery_id"] = "legacy:42";
  position(d, "kobo", false, "999999", 70);
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  ASSERT_TRUE(s.valid);
  auto e = event(s);
  EXPECT_STREQ(e["origin_device"]["id"], "kindle");
}
TEST(TotoProgressRecovery, EmptyAndOnlyOwnDoNotFallbackToOwnPosition) {
  auto d = response();
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  EXPECT_TRUE(s.valid);
  EXPECT_TRUE(s.eventJson.empty());
  position(d, "x4", true, "85", 0);
  s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  EXPECT_TRUE(s.valid);
  EXPECT_TRUE(s.eventJson.empty());
}
TEST(TotoProgressRecovery, RejectsAnotherEditionAndMalformedIdentity) {
  auto d = response();
  position(d, "kindle", false, "70", 30);
  d["positions"][0]["book_identifier"]["value"] = "ffffffffffffffffffffffffffffffff";
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), "../bad", true).valid);
}
TEST(TotoProgressRecovery, RejectsMalformedEnvelopeInsteadOfClaimingNoProgress) {
  auto d = response();
  d["positions"] = nullptr;
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
  d = response();
  d["protocol_version"] = 1;
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
}
TEST(TotoProgressRecovery, RejectsInvalidPercentageAndUnidentifiedLegacyRevision) {
  auto d = response();
  position(d, "kindle", false, nullptr, 30);
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
  d["positions"][0]["revision"] = "70";
  d["positions"][0]["percentage"] = 101;
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
  d["positions"][0]["percentage"] = "30";
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
}
TEST(TotoProgressRecovery, PercentageFallbackAcceptsNullLocatorAndName) {
  auto d = response();
  position(d, "kindle", false, "70", 30);
  d["positions"][0]["xpointer"] = nullptr;
  d["positions"][0]["device_name"] = nullptr;
  auto s = toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true);
  ASSERT_TRUE(s.valid);
  auto e = event(s);
  EXPECT_STREQ(e["payload"]["xpointer"], "");
}
TEST(TotoProgressRecovery, ValidatesWholeResponseBeforeOfferingCandidate) {
  auto d = response();
  position(d, "kindle", false, "70", 30);
  position(d, "x4", true, "60", 0);
  d["positions"][1]["is_current_device"] = nullptr;
  EXPECT_FALSE(toto::selectRecoveryPosition(d.as<JsonObjectConst>(), hash, true).valid);
}
}  // namespace
