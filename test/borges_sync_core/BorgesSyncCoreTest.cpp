#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "BorgesSyncCore.h"

namespace {

std::vector<uint8_t> bytes(const borges::Checkpoint& checkpoint) {
  const auto encoded = borges::encodeCheckpoint(checkpoint);
  return {encoded.begin(), encoded.end()};
}

TEST(BorgesCheckpoint, RoundTripsAllFields) {
  const borges::Checkpoint expected{
      .generation = 42,
      .nextSequence = 9007199254740993ULL,
      .appliedCursor = 700,
      .wallAnchorUnixSeconds = 1784894400,
      .monotonicAnchorMs = 0xFFFFFF00U,
      .bootId = 1234,
      .anchorPrecision = borges::TimePrecision::EXACT,
  };
  const auto encoded = borges::encodeCheckpoint(expected);
  const auto decoded = borges::decodeCheckpoint(encoded.data(), encoded.size());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, expected);
}

TEST(BorgesCheckpoint, RejectsTruncationAndCorruption) {
  borges::Checkpoint checkpoint;
  auto encoded = bytes(checkpoint);
  EXPECT_FALSE(borges::decodeCheckpoint(encoded.data(), encoded.size() - 1).has_value());
  encoded[20] ^= 0x80U;
  EXPECT_FALSE(borges::decodeCheckpoint(encoded.data(), encoded.size()).has_value());
}

TEST(BorgesCheckpoint, SelectsNewestValidSlotAndSurvivesWrap) {
  borges::Checkpoint old;
  old.generation = 0xFFFFFFFFU;
  borges::Checkpoint current;
  current.generation = 0;
  current.nextSequence = 9;
  auto damaged = bytes(old);
  damaged.back() ^= 1U;

  ASSERT_EQ(borges::selectNewestCheckpoint(bytes(old), bytes(current))->nextSequence, 9U);
  ASSERT_EQ(borges::selectNewestCheckpoint(damaged, bytes(current))->nextSequence, 9U);
}

TEST(BorgesBackoff, IsBoundedAndJittered) {
  EXPECT_EQ(borges::exponentialBackoffMs(0, 0), borges::BACKOFF_BASE_MS);
  EXPECT_GT(borges::exponentialBackoffMs(1, 1000), borges::BACKOFF_BASE_MS * 2U);
  EXPECT_LE(borges::exponentialBackoffMs(255, UINT32_MAX), borges::BACKOFF_MAX_MS);
}

TEST(BorgesTime, UsesAnchorAcrossMillisWrapAndDegradesAfterColdBoot) {
  borges::Checkpoint checkpoint;
  checkpoint.wallAnchorUnixSeconds = 1000;
  checkpoint.monotonicAnchorMs = 0xFFFFFF00U;
  checkpoint.bootId = 7;

  const auto anchored = borges::resolveWallTime(checkpoint, 7, 0x000003E8U);
  EXPECT_EQ(anchored.unixSeconds, 1001U);
  EXPECT_EQ(anchored.precision, borges::TimePrecision::ANCHORED);

  const auto approximate = borges::resolveWallTime(checkpoint, 8, 5000);
  EXPECT_EQ(approximate.unixSeconds, 1000U);
  EXPECT_EQ(approximate.precision, borges::TimePrecision::APPROXIMATE);
  EXPECT_EQ(borges::formatRfc3339(0), "1970-01-01T00:00:00Z");
  ASSERT_TRUE(borges::parseRfc3339Utc("2026-07-24T12:34:56.000Z").has_value());
  EXPECT_EQ(borges::formatRfc3339(*borges::parseRfc3339Utc("2026-07-24T12:34:56.000Z")), "2026-07-24T12:34:56Z");
  EXPECT_FALSE(borges::parseRfc3339Utc("not-a-time").has_value());
  EXPECT_FALSE(borges::parseRfc3339Utc("2026-02-29T12:34:56Z").has_value());
  EXPECT_FALSE(borges::parseRfc3339Utc("2026-07-24T12:34:56.badZ").has_value());
  EXPECT_FALSE(borges::parseRfc3339Utc("2026-07-24T12:34:60Z").has_value());
}

TEST(BorgesUuid, FormatsRandomAndDeterministicIdentifiers) {
  std::array<uint8_t, 16> random{};
  const auto randomId = borges::formatUuidV4(random);
  EXPECT_EQ(randomId, "00000000-0000-4000-8000-000000000000");
  EXPECT_TRUE(borges::isUuid(randomId));

  const auto first = borges::deterministicUuid("session:book:page:3");
  const auto second = borges::deterministicUuid("session:book:page:3");
  EXPECT_EQ(first, second);
  EXPECT_EQ(first[14], '5');
  EXPECT_NE(first, borges::deterministicUuid("session:book:page:4"));
}

TEST(BorgesAcknowledgment, RequiresExactIdAndDecimalSequence) {
  EXPECT_TRUE(borges::acknowledgmentMatches("event", 42, "event", "42"));
  EXPECT_FALSE(borges::acknowledgmentMatches("event", 42, "other", "42"));
  EXPECT_FALSE(borges::acknowledgmentMatches("event", 42, "event", "0043"));
  EXPECT_FALSE(borges::acknowledgmentMatches("event", 42, "event", "42x"));
}

TEST(BorgesProgress, EveryRemotePositionRequiresUserConsent) {
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("apply_initial")));
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("merge_forward")));
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("equivalent")));
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("suggest_resume")));
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("keep_local")));
  EXPECT_FALSE(borges::shouldAutoApply(borges::parseProgressDirective("suggest_jump")));
  EXPECT_EQ(borges::parseProgressDirective("future_value"), borges::ProgressDirective::UNKNOWN);
}

TEST(BorgesScheduler, ReaderWindowDebouncesAndHonorsCooldownAcrossMillisWrap) {
  EXPECT_FALSE(borges::readerSyncWindowReady(10'000, 9'000, 0, false, false));
  EXPECT_FALSE(borges::readerSyncWindowReady(10'000, 9'000, 0, true, true));
  EXPECT_TRUE(borges::readerSyncWindowReady(10'000, 9'000, 0, true, false));
  EXPECT_FALSE(borges::readerSyncWindowReady(10'000, 11'000, 0, true, false));
  EXPECT_FALSE(borges::readerSyncWindowReady(100'000, 90'000, 99'000, true, false));
  EXPECT_TRUE(borges::readerSyncWindowReady(300'000, 90'000, 100'000, true, false));

  constexpr uint32_t nearWrap = 0xfffffff0U;
  EXPECT_FALSE(borges::deadlineReached(nearWrap, 0x00000010U));
  EXPECT_TRUE(borges::deadlineReached(0x00000020U, 0x00000010U));
}

TEST(BorgesAnnotations, BoundsUtf8AndBuildsStableIdentities) {
  const std::string accented = "página sincronizada";
  EXPECT_EQ(borges::boundedUtf8(accented, 2), "p");
  EXPECT_EQ(borges::boundedUtf8(accented, 7), "página");
  EXPECT_EQ(borges::boundedUtf8(accented, 100), accented);

  const auto syncId = borges::legacyBookmarkSyncId("abcdef", "/body/p[1]", 1250);
  EXPECT_TRUE(borges::isUuid(syncId));
  EXPECT_EQ(syncId, borges::legacyBookmarkSyncId("abcdef", "/body/p[1]", 1250));
  EXPECT_NE(syncId, borges::legacyBookmarkSyncId("abcdef", "/body/p[2]", 1250));

  const auto first = borges::annotationFingerprint(syncId, "passage", "note", "/body/p[1]", 1250);
  EXPECT_EQ(first, borges::annotationFingerprint(syncId, "passage", "note", "/body/p[1]", 1250));
  EXPECT_NE(first, borges::annotationFingerprint(syncId, "passage", "edited", "/body/p[1]", 1250));
}

}  // namespace

TEST(BorgesProgress, FailedOrIncompletePullNeverPermitsAnUpload) {
  EXPECT_FALSE(borges::mayUploadAfterPull(false, false));
  EXPECT_FALSE(borges::mayUploadAfterPull(false, true));
  EXPECT_FALSE(borges::mayUploadAfterPull(true, true));
  EXPECT_TRUE(borges::mayUploadAfterPull(true, false));
}
