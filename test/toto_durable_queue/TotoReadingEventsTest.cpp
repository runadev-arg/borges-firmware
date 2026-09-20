#include <HalStorage.h>
#include <gtest/gtest.h>

#include "TotoCredentialStore.h"
#include "TotoReadingEvents.h"
#include "TotoSyncScheduler.h"

namespace toto {
void CredentialStore::setCredential(std::string id, std::string secret) {
  deviceId = id;
  token = secret;
}
SyncScheduler& SyncScheduler::instance() {
  static SyncScheduler scheduler;
  return scheduler;
}
void SyncScheduler::notifyLifecycleCommit() {}
}  // namespace toto

class TotoReadingPersistence : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.failOpen = false;
    Storage.failRename = false;
    TOTO_READING_EVENTS.endBook();
    Storage.files.clear();
    TOTO_CREDENTIALS.setCredential("test-device", "fixture");
  }
};

TEST_F(TotoReadingPersistence, ReopeningAndRerenderingDoNotPublishOldProgress) {
  ASSERT_TRUE(TOTO_READING_EVENTS.beginBook("book.epub", "Book", "Author"));
  const auto baselineDepth = TOTO_QUEUE.depth();
  ASSERT_TRUE(TOTO_READING_EVENTS.recordPosition(0.5f, "/p[4]"));
  EXPECT_EQ(TOTO_QUEUE.depth(), baselineDepth);
  ASSERT_TRUE(TOTO_READING_EVENTS.recordPosition(0.5f, "/p[4]"));
  EXPECT_EQ(TOTO_QUEUE.depth(), baselineDepth);
  ASSERT_TRUE(TOTO_READING_EVENTS.recordPosition(0.55f, "/p[5]"));
  EXPECT_GT(TOTO_QUEUE.depth(), baselineDepth);
}

TEST_F(TotoReadingPersistence, FailedWriteRemainsRetryableWithoutAnotherPageTurn) {
  ASSERT_TRUE(TOTO_READING_EVENTS.beginBook("book.epub", "Book", "Author"));
  ASSERT_TRUE(TOTO_READING_EVENTS.recordPosition(0.5f, "/p[4]"));
  Storage.failOpen = true;
  EXPECT_FALSE(TOTO_READING_EVENTS.recordPosition(0.55f, "/p[5]"));
  EXPECT_TRUE(TOTO_READING_EVENTS.hasPendingProgress());
  EXPECT_FALSE(TOTO_READING_EVENTS.endBook());
  EXPECT_FALSE(TOTO_READING_EVENTS.beginBook("other.epub", "Other", "Author"));
  EXPECT_EQ(TOTO_READING_EVENTS.getBookHash(), std::string(32, '1'));
  Storage.failOpen = false;
  ASSERT_TRUE(TOTO_READING_EVENTS.retryPendingProgress());
  EXPECT_FALSE(TOTO_READING_EVENTS.hasPendingProgress());
  bool found = false;
  for (const auto& event : TOTO_QUEUE.nextBatch(20, 20000)) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, event.body));
    if (std::string(doc["event_type"] | "") == "progress.changed") {
      EXPECT_NEAR(doc["payload"]["percentage"].as<double>(), 55.0, 0.001);
      EXPECT_EQ(std::string(doc["payload"]["xpointer"] | ""), "/p[5]");
      found = true;
    }
  }
  EXPECT_TRUE(found);
  ASSERT_TRUE(TOTO_READING_EVENTS.endBook());
  ASSERT_TRUE(TOTO_READING_EVENTS.beginBook("other.epub", "Other", "Author"));
  EXPECT_EQ(TOTO_READING_EVENTS.getBookHash(), std::string(32, '2'));
}

TEST_F(TotoReadingPersistence, AcceptedRemoteIntentSurvivesFailedQueueAndRerender) {
  ASSERT_TRUE(TOTO_READING_EVENTS.beginBook("book.epub", "Book", "Author"));
  const std::string event =
      R"({"event_type":"progress.changed","directive":"suggest_jump","book_identifier":{"value":"11111111111111111111111111111111"},"payload":{"percentage":72,"xpointer":"/p[9]"}})";
  ASSERT_TRUE(TOTO_QUEUE.deferPulled(400, event));
  auto remote = TOTO_QUEUE.nextProgressDecision(TOTO_READING_EVENTS.getBookHash());
  ASSERT_TRUE(remote);
  ASSERT_TRUE(TOTO_QUEUE.acceptProgress(*remote));
  Storage.failOpen = true;
  EXPECT_FALSE(TOTO_READING_EVENTS.recordPosition(0.72f, "/p[9]", true));
  EXPECT_FALSE(TOTO_READING_EVENTS.recordPosition(0.72f, "/p[9]"));
  Storage.failOpen = false;
  // The accepted file is durable even though there is no outgoing event yet.
  auto accepted = TOTO_QUEUE.nextApplicableProgress(TOTO_READING_EVENTS.getBookHash());
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->serverSequence, 400);
  EXPECT_FALSE(TOTO_QUEUE.nextProgressDecision(TOTO_READING_EVENTS.getBookHash()));
  ASSERT_TRUE(TOTO_READING_EVENTS.retryPendingProgress());
  bool found = false;
  for (const auto& queued : TOTO_QUEUE.nextBatch(20, 20000)) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, queued.body));
    if (std::string(doc["event_type"] | "") == "progress.changed") {
      EXPECT_TRUE(doc["payload"]["explicit_jump"].as<bool>());
      EXPECT_NEAR(doc["payload"]["percentage"].as<double>(), 72.0, 0.001);
      found = true;
    }
  }
  ASSERT_TRUE(found);
  ASSERT_TRUE(TOTO_QUEUE.resolveProgressThrough(TOTO_READING_EVENTS.getBookHash(), 400));
  EXPECT_FALSE(TOTO_QUEUE.nextApplicableProgress(TOTO_READING_EVENTS.getBookHash()));
}
