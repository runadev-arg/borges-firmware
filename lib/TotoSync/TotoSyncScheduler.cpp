#include "TotoSyncScheduler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>

#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoResumeFlow.h"
#include "TotoResumeStore.h"
#include "TotoSyncClient.h"
#include "TotoSyncCore.h"

namespace toto {

SyncScheduler& SyncScheduler::instance() {
  static SyncScheduler scheduler;
  return scheduler;
}

void SyncScheduler::notifyLifecycleCommit() {
  pending = true;
  if (nextAttemptMs == 0) nextAttemptMs = millis() + 5000U;
}

void SyncScheduler::tick(bool readerActive) {
  constexpr uint32_t CONNECT_DELAY_MS = 2000U;
  constexpr uint32_t IDLE_POLL_INTERVAL_MS = 15U * 60U * 1000U;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wifiWasConnected && TOTO_CREDENTIALS.paired()) {
    pending = true;
    // Ask before sending. Draining first tells the hub this reader's stale
    // offline position is the newest write, and the position the other device
    // left stops being offered at all -- the reader never sees the question.
    askBeforeSending = true;
    nextAttemptMs = millis() + CONNECT_DELAY_MS;
  } else if (wifiConnected && !pending && lastSuccessMs != 0 &&
             static_cast<uint32_t>(millis() - lastSuccessMs) >= IDLE_POLL_INTERVAL_MS) {
    pending = true;
    nextAttemptMs = millis();
  }
  wifiWasConnected = wifiConnected;

  if (!pending || static_cast<int32_t>(millis() - nextAttemptMs) < 0) return;

  const ConnectState state{
      .connected = wifiConnected,
      .hasSession = TOTO_CREDENTIALS.paired(),
      .running = running,
      .autoPull = true,
  };
  const ConnectPlan plan = planConnect(state);
  if (plan.skip != ConnectSkip::None) return;

  // Both steps are planned, so the first request of this reconnection is the
  // pull; the drain is the next tick's.
  const bool pullStep = askBeforeSending && plan.pullsBeforeDrain();
  // With a book open only that first question runs, once per reconnection.
  // Without it the reader has to close the book before they can be told their
  // Kobo moved the position -- which is the whole scenario. The queue, which
  // has no deadline, waits for the book to be closed.
  if (readerActive && !pullStep) return;

  running = true;
  const SyncClient::Outcome outcome = SyncClient::syncOnce(pullStep);
  running = false;
  if (outcome.result == SyncClient::Result::OK) {
    failures = 0;
    lastSuccessMs = millis();
    ++completedSyncs;
    // The pull came back, so the question the reader is about to be shown is
    // already on the card. Now the queue may drain -- and the answers given
    // with no signal can be delivered, once the book is closed.
    if (!readerActive) SyncClient::flushPendingResolutions();
    if (pullStep) askBeforeSending = false;
    const bool answersPending = !TOTO_RESUME.decisions().pendingResolutions().empty();
    pending = answersPending || outcome.queueDepth > 0 || outcome.hasMore;
    nextAttemptMs = pending ? millis() + 2000U : 0;
    return;
  }

  failures = std::min<uint8_t>(static_cast<uint8_t>(failures + 1U), 32U);
  nextAttemptMs = millis() + exponentialBackoffMs(failures, esp_random());
}

}  // namespace toto
