#include <HalStorage.h>
#include <gtest/gtest.h>

#include "DownloadPolicy.h"
#include "TotoDurableQueue.h"

namespace {
constexpr char book[] = "11111111111111111111111111111111";
std::string event(const char* directive, int percentage = 72) {
  return std::string("{\"event_type\":\"progress.changed\",\"directive\":\"") + directive +
         "\",\"book_identifier\":{\"value\":\"" + book +
         "\"},\"payload\":{\"percentage\":" + std::to_string(percentage) +
         ",\"xpointer\":\"/body/DocFragment[7]/body/p[4]\"}}";
}

std::string manualEvent(int percentage = 72) {
  return std::string("{\"event_type\":\"progress.changed\",\"directive\":\"own\",\"book_identifier\":{") +
         "\"kind\":\"koreader_partial_md5\",\"value\":\"" + book +
         "\"},\"origin_device\":{\"name\":\"Kobo\"},\"source_revision\":\"9999\",\"server_sequence\":null," +
         "\"payload\":{\"percentage\":" + std::to_string(percentage) + ",\"xpointer\":\"/p[8]\"}}";
}

class TotoInbox : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.files.clear();
    Storage.failRename = false;
    Storage.failOpen = false;
    Storage.renameCalls = 0;
    Storage.failRenameFromCall = 0;
    ASSERT_TRUE(TOTO_QUEUE.begin());
  }
};

TEST_F(TotoInbox, InitialAndSmallChangesAreRetainedForConsent) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(100, event("apply_initial")));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->percentage, 72);
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(101, event("merge_forward", 73)));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->percentage, 73);
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
}

TEST_F(TotoInbox, DecliningSurvivesReplayAndCanBeAcceptedOffline) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(200, event("suggest_resume")));
  const auto pending = TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(*pending));
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision(book));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(200, event("suggest_resume")));
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision(book));
  const auto saved = TOTO_QUEUE.nextProgressDecision(book, true);
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved->dismissed);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(*saved));
  ASSERT_TRUE(TOTO_QUEUE.nextApplicableProgress(book));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->percentage, 72);
}

TEST_F(TotoInbox, FailedAcceptanceKeepsTheRemoteLocation) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(300, event("suggest_jump")));
  const auto pending = TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(pending);
  Storage.failRename = true;
  EXPECT_FALSE(TOTO_QUEUE.acceptProgress(*pending));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->serverSequence, 300);
}

TEST_F(TotoInbox, OtherBooksDoNotMoveTheOpenBook) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(400, event("suggest_jump")));
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision("22222222222222222222222222222222"));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress("22222222222222222222222222222222"));
}

TEST_F(TotoInbox, FailedStorageDoesNotConsumeTheOutboxEvent) {
  const auto identity = TOTO_QUEUE.reserveIdentity("12345678-1234-4234-8234-123456789abc");
  ASSERT_TRUE(identity);
  Storage.failOpen = true;
  EXPECT_FALSE(TOTO_QUEUE.enqueue(*identity, "{\"test\":true}"));
  Storage.failOpen = false;
  ASSERT_TRUE(TOTO_QUEUE.enqueue(*identity, "{\"test\":true}"));
  EXPECT_EQ(TOTO_QUEUE.depth(), 1);
}

TEST_F(TotoInbox, ManualRecoveryIsIndependentOfCursorAndStreamRevisions) {
  const auto beforeCursor = TOTO_QUEUE.checkpoint().appliedCursor;
  ASSERT_TRUE(TOTO_QUEUE.commitAppliedCursor(beforeCursor + 117));
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(700, event("suggest_resume", 90)));
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto manual = TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(manual);
  EXPECT_TRUE(manual->manualRecovery);
  EXPECT_EQ(manual->serverSequence, 0);
  EXPECT_EQ(manual->percentage, 72);
  EXPECT_EQ(manual->sourceDeviceName, "Kobo");
  EXPECT_EQ(TOTO_QUEUE.checkpoint().appliedCursor, beforeCursor + 117);
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(*manual));
  ASSERT_TRUE(TOTO_QUEUE.nextApplicableProgress(book));
  EXPECT_TRUE(TOTO_QUEUE.nextApplicableProgress(book)->manualRecovery);
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(*manual));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->serverSequence, 700);
  EXPECT_EQ(TOTO_QUEUE.checkpoint().appliedCursor, beforeCursor + 117);
  // A position with no stream sequence can be fetched again after consumption.
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  EXPECT_NE(TOTO_QUEUE.nextProgressDecision(book)->recoveryRequestId, manual->recoveryRequestId);
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
}

TEST_F(TotoInbox, ManualRecoveryCanBeOfferedAgainAfterDismissalAndRejectsStaleConsent) {
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto first = *TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(first));
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision(book));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book, true));
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(40)));
  const auto latest = *TOTO_QUEUE.nextProgressDecision(book);
  EXPECT_NE(latest.recoveryRequestId, first.recoveryRequestId);
  EXPECT_EQ(latest.percentage, 40);
  EXPECT_FALSE(TOTO_QUEUE.acceptProgress(first));
  EXPECT_FALSE(TOTO_QUEUE.dismissProgress(first));
  EXPECT_FALSE(TOTO_QUEUE.resolveProgress(first));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(latest));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->percentage, 40);
}

TEST_F(TotoInbox, FailedManualReplacementOrDecisionKeepsPreviousDurableIntent) {
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto first = *TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(first));
  Storage.failOpen = true;
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(10)));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->recoveryRequestId, first.recoveryRequestId);
  Storage.failOpen = false;
  Storage.failRename = true;
  EXPECT_FALSE(TOTO_QUEUE.dismissProgress(first));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->recoveryRequestId, first.recoveryRequestId);
  Storage.failRename = false;
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(first));
}

TEST_F(TotoInbox, ManualRecoveryByBookIsFoundBeyondBoundedInboxScan) {
  for (uint64_t sequence = 1; sequence <= toto::DurableQueue::MAX_QUEUE_SCAN + 5; ++sequence) {
    ASSERT_TRUE(TOTO_QUEUE.deferPulled(sequence, "{\"event_type\":\"annotation.created\"}"));
  }
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book));
  EXPECT_TRUE(TOTO_QUEUE.nextProgressDecision(book)->manualRecovery);
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision("22222222222222222222222222222222"));
}

TEST_F(TotoInbox, ManualRecoveryRejectsWrongEditionAndInvalidPositionWithoutReplacingGoodCopy) {
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto first = *TOTO_QUEUE.nextProgressDecision(book);
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate("22222222222222222222222222222222", manualEvent()));
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate("../../escape", manualEvent()));
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, event("suggest_resume")));
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(101)));
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(-1)));
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, "{\"event_type\":\"progress.changed\"}"));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->recoveryRequestId, first.recoveryRequestId);
}

TEST_F(TotoInbox, ManualRequestSupersedesOldConsentButDoesNotHideFutureStreamSuggestions) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(20, event("suggest_resume", 15)));
  const auto old = *TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(old));
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(*TOTO_QUEUE.nextProgressDecision(book)));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  // Old intent is retained; the durable manual barrier prevents its activation.
  EXPECT_TRUE(Storage.exists("/.crosspoint/toto/inbox/00000000000000000020.accepted"));
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(30, event("suggest_resume", 80)));
  const auto later = TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(later);
  EXPECT_FALSE(later->manualRecovery);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(*later));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->serverSequence, 30);
}

TEST_F(TotoInbox, InterruptedManualReplacementAndFailedRollbackStillExposeCompleteBackup) {
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto first = *TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(first));
  Storage.renameCalls = 0;
  Storage.failRenameFromCall = 2;
  EXPECT_FALSE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(10)));
  ASSERT_TRUE(TOTO_QUEUE.nextApplicableProgress(book));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->recoveryRequestId, first.recoveryRequestId);
  Storage.failRenameFromCall = 0;
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(first));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book, true));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book, true)->recoveryRequestId, first.recoveryRequestId);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(first));
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(first));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book, true));
  EXPECT_TRUE(TOTO_QUEUE.nextProgressDecision(book, true)->recoveryResolved);
  EXPECT_FALSE(Storage.exists("/.crosspoint/toto/inbox/recovery_11111111111111111111111111111111.json.bak"));
}

TEST_F(TotoInbox, ResolvedOrDismissedManualRequestNeverRevivesHiddenOldConsent) {
  for (uint64_t sequence = 1; sequence <= toto::DurableQueue::MAX_QUEUE_SCAN + 5; ++sequence) {
    ASSERT_TRUE(TOTO_QUEUE.deferPulled(sequence, "{\"event_type\":\"annotation.created\"}"));
  }
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(500, event("suggest_resume", 15)));
  toto::ProgressInboxItem old;
  old.bookHash = book;
  old.serverSequence = 500;
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(old));
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  const auto manual = *TOTO_QUEUE.nextProgressDecision(book);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(manual));
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(manual));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent(40)));
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(*TOTO_QUEUE.nextProgressDecision(book)));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  // Previously hidden consent becomes visible after other inbox work is drained.
  for (uint64_t sequence = 1; sequence <= toto::DurableQueue::MAX_QUEUE_SCAN + 5; ++sequence) {
    ASSERT_TRUE(TOTO_QUEUE.resolveInbox(sequence));
  }
  EXPECT_TRUE(Storage.exists("/.crosspoint/toto/inbox/00000000000000000500.accepted"));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(700, event("suggest_resume", 80)));
  const auto chosen = *TOTO_QUEUE.nextProgressDecision(book);
  EXPECT_EQ(chosen.serverSequence, 700);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(chosen));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->serverSequence, 700);
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision(book));
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(chosen));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
}

TEST_F(TotoInbox, ExplicitStreamConsentAndResolutionRemainDurableBeyondScanLimit) {
  ASSERT_TRUE(TOTO_QUEUE.restoreProgressCandidate(book, manualEvent()));
  ASSERT_TRUE(TOTO_QUEUE.dismissProgress(*TOTO_QUEUE.nextProgressDecision(book)));
  for (uint64_t sequence = 1; sequence <= toto::DurableQueue::MAX_QUEUE_SCAN + 5; ++sequence) {
    ASSERT_TRUE(TOTO_QUEUE.deferPulled(sequence, "{\"event_type\":\"annotation.created\"}"));
  }
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(700, event("suggest_resume", 80)));
  toto::ProgressInboxItem chosen;
  chosen.bookHash = book;
  chosen.serverSequence = 700;
  Storage.failRename = true;
  EXPECT_FALSE(TOTO_QUEUE.acceptProgress(chosen));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
  Storage.failRename = false;
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(chosen));
  EXPECT_EQ(TOTO_QUEUE.nextApplicableProgress(book)->serverSequence, 700);
  ASSERT_TRUE(TOTO_QUEUE.resolveProgress(chosen));
  EXPECT_FALSE(Storage.exists("/.crosspoint/toto/inbox/00000000000000000700.json"));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(book));
}

TEST_F(TotoInbox, DirectStreamResolutionCannotRemoveAnotherBooksCandidate) {
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(700, event("suggest_resume", 80)));
  toto::ProgressInboxItem wrongBook;
  wrongBook.bookHash = "22222222222222222222222222222222";
  wrongBook.serverSequence = 700;
  EXPECT_FALSE(TOTO_QUEUE.resolveProgress(wrongBook));
  ASSERT_TRUE(TOTO_QUEUE.nextProgressDecision(book));
  EXPECT_EQ(TOTO_QUEUE.nextProgressDecision(book)->serverSequence, 700);
}

TEST(DownloadPolicy, PairedTokenCannotFollowAnotherOriginOrDowngrade) {
  constexpr auto start = "https://highlights.runadev.com/api/opds";
  EXPECT_TRUE(download_policy::safeRedirect(start, "https://highlights.runadev.com/api/opds/books/1", true));
  EXPECT_FALSE(download_policy::safeRedirect(start, "http://highlights.runadev.com/api/opds", true));
  EXPECT_FALSE(download_policy::safeRedirect(start, "https://evil.example/api/opds", true));
  EXPECT_FALSE(download_policy::safeRedirect(start, "https://highlights.runadev.com.evil.example/book", true));
  EXPECT_TRUE(download_policy::safeRedirect(start, "https://cdn.example/book", false));
}
}  // namespace

#include "TotoPullValidation.h"
TEST(TotoPullResponse, RejectsMissingFieldsBeforeAnyOutboxUpload) {
  const char* invalid[] = {
      R"({"cursor":"5","high_watermark":"5"})",
      R"({"cursor":"5","high_watermark":"5","events":[],"has_more":"false"})",
      R"({"cursor":"5","high_watermark":"6","events":[],"has_more":false})",
      R"({"cursor":"5","high_watermark":"5","events":[null],"has_more":false})",
      R"({"cursor":"5","high_watermark":"5","events":[{"event_type":"progress.changed","server_sequence":"6"}],"has_more":false})",
      R"({"cursor":"4","high_watermark":"5","events":[],"has_more":true})"};
  for (const char* json : invalid) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, json));
    EXPECT_FALSE(toto::validatePull(doc.as<JsonObjectConst>(), 4)) << json;
  }
}
TEST(TotoPullResponse, AcceptsCompleteSnapshotAndBoundedPages) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"cursor":"5","high_watermark":"5","events":[],"has_more":false})"));
  auto complete = toto::validatePull(doc.as<JsonObjectConst>(), 4);
  ASSERT_TRUE(complete);
  EXPECT_FALSE(complete->hasMore);
  ASSERT_FALSE(deserializeJson(
      doc,
      R"({"cursor":"5","high_watermark":"6","events":[{"event_type":"progress.changed","server_sequence":"5"}],"has_more":true})"));
  auto page = toto::validatePull(doc.as<JsonObjectConst>(), 4);
  ASSERT_TRUE(page);
  EXPECT_TRUE(page->hasMore);
}
