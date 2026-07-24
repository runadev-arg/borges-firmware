#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "TotoSyncCore.h"

namespace {

std::vector<uint8_t> bytes(const toto::Checkpoint& checkpoint) {
  const auto encoded = toto::encodeCheckpoint(checkpoint);
  return {encoded.begin(), encoded.end()};
}

TEST(TotoCheckpoint, RoundTripsAllFields) {
  const toto::Checkpoint expected{
      .generation = 42,
      .nextSequence = 9007199254740993ULL,
      .appliedCursor = 700,
      .wallAnchorUnixSeconds = 1784894400,
      .monotonicAnchorMs = 0xFFFFFF00U,
      .bootId = 1234,
      .anchorPrecision = toto::TimePrecision::EXACT,
  };
  const auto encoded = toto::encodeCheckpoint(expected);
  const auto decoded = toto::decodeCheckpoint(encoded.data(), encoded.size());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, expected);
}

TEST(TotoCheckpoint, RejectsTruncationAndCorruption) {
  toto::Checkpoint checkpoint;
  auto encoded = bytes(checkpoint);
  EXPECT_FALSE(toto::decodeCheckpoint(encoded.data(), encoded.size() - 1).has_value());
  encoded[20] ^= 0x80U;
  EXPECT_FALSE(toto::decodeCheckpoint(encoded.data(), encoded.size()).has_value());
}

TEST(TotoCheckpoint, SelectsNewestValidSlotAndSurvivesWrap) {
  toto::Checkpoint old;
  old.generation = 0xFFFFFFFFU;
  toto::Checkpoint current;
  current.generation = 0;
  current.nextSequence = 9;
  auto damaged = bytes(old);
  damaged.back() ^= 1U;

  ASSERT_EQ(toto::selectNewestCheckpoint(bytes(old), bytes(current))->nextSequence, 9U);
  ASSERT_EQ(toto::selectNewestCheckpoint(damaged, bytes(current))->nextSequence, 9U);
}

TEST(TotoBackoff, IsBoundedAndJittered) {
  EXPECT_EQ(toto::exponentialBackoffMs(0, 0), toto::BACKOFF_BASE_MS);
  EXPECT_GT(toto::exponentialBackoffMs(1, 1000), toto::BACKOFF_BASE_MS * 2U);
  EXPECT_LE(toto::exponentialBackoffMs(255, UINT32_MAX), toto::BACKOFF_MAX_MS);
}

TEST(TotoTime, UsesAnchorAcrossMillisWrapAndDegradesAfterColdBoot) {
  toto::Checkpoint checkpoint;
  checkpoint.wallAnchorUnixSeconds = 1000;
  checkpoint.monotonicAnchorMs = 0xFFFFFF00U;
  checkpoint.bootId = 7;

  const auto anchored = toto::resolveWallTime(checkpoint, 7, 0x000003E8U);
  EXPECT_EQ(anchored.unixSeconds, 1001U);
  EXPECT_EQ(anchored.precision, toto::TimePrecision::ANCHORED);

  const auto approximate = toto::resolveWallTime(checkpoint, 8, 5000);
  EXPECT_EQ(approximate.unixSeconds, 1000U);
  EXPECT_EQ(approximate.precision, toto::TimePrecision::APPROXIMATE);
  EXPECT_EQ(toto::formatRfc3339(0), "1970-01-01T00:00:00Z");
  ASSERT_TRUE(toto::parseRfc3339Utc("2026-07-24T12:34:56.000Z").has_value());
  EXPECT_EQ(toto::formatRfc3339(*toto::parseRfc3339Utc("2026-07-24T12:34:56.000Z")), "2026-07-24T12:34:56Z");
  EXPECT_FALSE(toto::parseRfc3339Utc("not-a-time").has_value());
  EXPECT_FALSE(toto::parseRfc3339Utc("2026-02-29T12:34:56Z").has_value());
  EXPECT_FALSE(toto::parseRfc3339Utc("2026-07-24T12:34:56.badZ").has_value());
  EXPECT_FALSE(toto::parseRfc3339Utc("2026-07-24T12:34:60Z").has_value());
}

TEST(TotoUuid, FormatsRandomAndDeterministicIdentifiers) {
  std::array<uint8_t, 16> random{};
  const auto randomId = toto::formatUuidV4(random);
  EXPECT_EQ(randomId, "00000000-0000-4000-8000-000000000000");
  EXPECT_TRUE(toto::isUuid(randomId));

  const auto first = toto::deterministicUuid("session:book:page:3");
  const auto second = toto::deterministicUuid("session:book:page:3");
  EXPECT_EQ(first, second);
  EXPECT_EQ(first[14], '5');
  EXPECT_NE(first, toto::deterministicUuid("session:book:page:4"));
}

TEST(TotoAcknowledgment, RequiresExactIdAndDecimalSequence) {
  EXPECT_TRUE(toto::acknowledgmentMatches("event", 42, "event", "42"));
  EXPECT_FALSE(toto::acknowledgmentMatches("event", 42, "other", "42"));
  EXPECT_FALSE(toto::acknowledgmentMatches("event", 42, "event", "0043"));
  EXPECT_FALSE(toto::acknowledgmentMatches("event", 42, "event", "42x"));
}

TEST(TotoProgress, OnlySafeDirectivesAutoApply) {
  EXPECT_TRUE(toto::shouldAutoApply(toto::parseProgressDirective("apply_initial")));
  EXPECT_TRUE(toto::shouldAutoApply(toto::parseProgressDirective("merge_forward")));
  EXPECT_FALSE(toto::shouldAutoApply(toto::parseProgressDirective("keep_local")));
  EXPECT_FALSE(toto::shouldAutoApply(toto::parseProgressDirective("suggest_resume")));
  EXPECT_EQ(toto::parseProgressDirective("future_value"), toto::ProgressDirective::UNKNOWN);
}

}  // namespace
