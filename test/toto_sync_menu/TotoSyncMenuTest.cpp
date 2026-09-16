#include <gtest/gtest.h>

#include <string>

#include "TotoSyncMenu.h"

namespace {

constexpr uint64_t NOW = 1789000000;  // 2026-09-xx, well past the trusted-clock floor

toto::SyncSnapshot signedIn() {
  toto::SyncSnapshot snapshot;
  snapshot.signedIn = true;
  snapshot.session = toto::SessionState::ACTIVE;
  snapshot.now = NOW;
  snapshot.lastSuccessAt = NOW;
  return snapshot;
}

TEST(TotoMenuStatus, SignedOutOutranksEverythingElse) {
  toto::SyncSnapshot snapshot;
  snapshot.outbox = 7;
  snapshot.remotePositionWaiting = true;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::SignedOut);

  snapshot.pairingPending = true;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::PairingPending);
}

TEST(TotoMenuStatus, ExpiredSessionBeatsQueueAndSuggestion) {
  toto::SyncSnapshot snapshot = signedIn();
  snapshot.outbox = 3;
  snapshot.remotePositionWaiting = true;

  snapshot.session = toto::SessionState::EXPIRED;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::SessionExpired);

  snapshot.session = toto::SessionState::RENEW_DUE;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::SessionRenewDue);
}

TEST(TotoMenuStatus, WaitingPositionIsLouderThanAPendingQueue) {
  toto::SyncSnapshot snapshot = signedIn();
  snapshot.outbox = 4;
  snapshot.remotePositionWaiting = true;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::RemotePositionWaiting);
}

TEST(TotoMenuStatus, CountsInboxAsPendingWorkNotAsUpToDate) {
  toto::SyncSnapshot snapshot = signedIn();
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::UpToDate);

  snapshot.inbox = 1;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::PendingUploads);

  snapshot.inbox = 0;
  snapshot.outbox = 1;
  EXPECT_EQ(toto::summarize(snapshot), toto::StatusKind::PendingUploads);
}

TEST(TotoMenuAge, NeverWithoutASuccessOrWithAClockBehindIt) {
  toto::SyncSnapshot snapshot = signedIn();
  snapshot.lastSuccessAt = 0;
  EXPECT_EQ(toto::lastSuccessAge(snapshot), (toto::Age{toto::AgeUnit::Never, 0}));

  // A reader that booted without NTP reports second 0. Saying "51 years ago"
  // would be worse than admitting the clock is unknown.
  snapshot.lastSuccessAt = NOW;
  snapshot.now = 0;
  EXPECT_EQ(toto::lastSuccessAge(snapshot), (toto::Age{toto::AgeUnit::Never, 0}));
}

TEST(TotoMenuAge, BucketsByTheUnitAReaderWouldUse) {
  toto::SyncSnapshot snapshot = signedIn();
  const auto ago = [&](uint64_t seconds) {
    snapshot.lastSuccessAt = NOW - seconds;
    return toto::lastSuccessAge(snapshot);
  };

  EXPECT_EQ(ago(0), (toto::Age{toto::AgeUnit::Moments, 0}));
  EXPECT_EQ(ago(59), (toto::Age{toto::AgeUnit::Moments, 0}));
  EXPECT_EQ(ago(60), (toto::Age{toto::AgeUnit::Minutes, 1}));
  EXPECT_EQ(ago(59 * 60), (toto::Age{toto::AgeUnit::Minutes, 59}));
  EXPECT_EQ(ago(3600), (toto::Age{toto::AgeUnit::Hours, 1}));
  EXPECT_EQ(ago(23 * 3600), (toto::Age{toto::AgeUnit::Hours, 23}));
  EXPECT_EQ(ago(24 * 3600), (toto::Age{toto::AgeUnit::Days, 1}));
  EXPECT_EQ(ago(9 * 24 * 3600), (toto::Age{toto::AgeUnit::Days, 9}));
}

TEST(TotoMenuRows, SignedOutOffersOnlyTheWaysIn) {
  const toto::SyncSnapshot snapshot;
  EXPECT_TRUE(toto::rowEnabled(toto::MenuRow::Account, snapshot));
  EXPECT_TRUE(toto::rowEnabled(toto::MenuRow::StatusHelp, snapshot));
  EXPECT_TRUE(toto::rowEnabled(toto::MenuRow::Advanced, snapshot));
  EXPECT_FALSE(toto::rowEnabled(toto::MenuRow::SyncNow, snapshot));
  EXPECT_FALSE(toto::rowEnabled(toto::MenuRow::Library, snapshot));
}

TEST(TotoMenuRows, SignedInOffersEveryRow) {
  const toto::SyncSnapshot snapshot = signedIn();
  for (int index = 0; index < toto::MENU_ROW_COUNT; ++index) {
    EXPECT_TRUE(toto::rowEnabled(static_cast<toto::MenuRow>(index), snapshot)) << "row " << index;
  }
}

TEST(TotoMenuRows, PairingAndRepairNeverOfferThemselvesAtTheSameTime) {
  toto::SyncSnapshot snapshot;
  EXPECT_TRUE(toto::advancedRowEnabled(toto::AdvancedRow::PairWithCode, snapshot));
  EXPECT_FALSE(toto::advancedRowEnabled(toto::AdvancedRow::RepairServices, snapshot));

  snapshot = signedIn();
  EXPECT_FALSE(toto::advancedRowEnabled(toto::AdvancedRow::PairWithCode, snapshot));
  EXPECT_TRUE(toto::advancedRowEnabled(toto::AdvancedRow::RepairServices, snapshot));
}

TEST(TotoMenuRows, DiscardingAPositionNeedsAPositionToDiscard) {
  toto::SyncSnapshot snapshot = signedIn();
  EXPECT_FALSE(toto::advancedRowEnabled(toto::AdvancedRow::DiscardRemotePosition, snapshot));

  snapshot.remotePositionWaiting = true;
  EXPECT_TRUE(toto::advancedRowEnabled(toto::AdvancedRow::DiscardRemotePosition, snapshot));
}

TEST(TotoReportCode, NamesTheFirmwareAndTheDeviceAndNothingElse) {
  const std::string code = toto::reportCode("1.4.2", "x4-8a1b2c3d4e5f");
  EXPECT_EQ(code, "1.4.2 / x4-8a1b2c3d4e5f");
}

TEST(TotoReportCode, StaysReadableWhenThePiecesAreMissingOrOversized) {
  EXPECT_EQ(toto::reportCode("", ""), "? / -");
  EXPECT_EQ(toto::reportCode("1.4.2", ""), "1.4.2 / -");

  const std::string longId(200, 'a');
  const std::string code = toto::reportCode("1.4.2", longId);
  EXPECT_LE(code.size(), 64u);
  EXPECT_EQ(code.rfind("1.4.2 / ", 0), 0u);
}

TEST(TotoSupportHost, DropsTheSchemeAndAnyPath) {
  EXPECT_EQ(toto::supportHost("https://highlights.runadev.com"), "highlights.runadev.com");
  EXPECT_EQ(toto::supportHost("https://highlights.runadev.com/api/sync"), "highlights.runadev.com");
  EXPECT_EQ(toto::supportHost("highlights.runadev.com"), "highlights.runadev.com");
  EXPECT_EQ(toto::supportHost(""), "");
}

}  // namespace
