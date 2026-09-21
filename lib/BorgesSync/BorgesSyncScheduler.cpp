#include "BorgesSyncScheduler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>

#include "BorgesCredentialStore.h"
#include "BorgesDurableQueue.h"
#include "BorgesReadingEvents.h"
#include "BorgesSyncClient.h"
#include "BorgesSyncCore.h"

namespace borges {

SyncScheduler& SyncScheduler::instance() {
  static SyncScheduler scheduler;
  return scheduler;
}

void SyncScheduler::notifyLifecycleCommit() {
  pending = true;
  if (nextAttemptMs == 0) nextAttemptMs = millis() + 5000U;
}

void SyncScheduler::refreshConnectivitySchedule() {
  constexpr uint32_t CONNECT_DELAY_MS = 2000U;
  constexpr uint32_t IDLE_POLL_INTERVAL_MS = 15U * 60U * 1000U;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wifiWasConnected && BORGES_CREDENTIALS.paired()) {
    pending = true;
    nextAttemptMs = millis() + CONNECT_DELAY_MS;
  } else if (wifiConnected && !pending && lastSuccessMs != 0 &&
             static_cast<uint32_t>(millis() - lastSuccessMs) >= IDLE_POLL_INTERVAL_MS) {
    pending = true;
    nextAttemptMs = millis();
  }
  wifiWasConnected = wifiConnected;
}

void SyncScheduler::tick(bool readerActive) {
  refreshConnectivitySchedule();

  if (running || readerActive || !pending || !BORGES_CREDENTIALS.paired() || WiFi.status() != WL_CONNECTED ||
      !deadlineReached(millis(), nextAttemptMs)) {
    return;
  }

  runBurst(1);
}

bool SyncScheduler::readerSyncDue(bool pageSettled, bool buildActive) {
  refreshConnectivitySchedule();
  return !running && pending && BORGES_CREDENTIALS.paired() && WiFi.status() == WL_CONNECTED &&
         readerSyncWindowReady(millis(), nextAttemptMs, lastSuccessMs, pageSettled, buildActive);
}

void SyncScheduler::syncNow(uint8_t maxExchanges) {
  refreshConnectivitySchedule();
  if (running || !BORGES_CREDENTIALS.paired() || WiFi.status() != WL_CONNECTED) return;
  runBurst(std::max<uint8_t>(maxExchanges, 1U));
}

void SyncScheduler::flushBeforeSleep() {
  if (!BORGES_CREDENTIALS.paired() || WiFi.status() != WL_CONNECTED || BORGES_QUEUE.depth() == 0) return;
  pending = true;
  nextAttemptMs = millis();
  runBurst(1);
}

void SyncScheduler::runBurst(uint8_t maxExchanges) {
  if (running) return;

  running = true;
  SyncClient::Outcome outcome;
  for (uint8_t exchange = 0; exchange < maxExchanges; ++exchange) {
    if (!BORGES_READING_EVENTS.retryPendingProgress()) {
      outcome.result = SyncClient::Result::STORAGE_ERROR;
      break;
    }
    outcome = SyncClient::syncOnce();
    if (outcome.result != SyncClient::Result::OK || (outcome.queueDepth == 0 && !outcome.hasMore)) break;
  }
  running = false;
  if (outcome.result == SyncClient::Result::OK) {
    failures = 0;
    lastSuccessMs = millis();
    pending = outcome.queueDepth > 0 || outcome.hasMore;
    nextAttemptMs = pending ? millis() + 2000U : 0;
    return;
  }

  failures = std::min<uint8_t>(static_cast<uint8_t>(failures + 1U), 32U);
  nextAttemptMs = millis() + exponentialBackoffMs(failures, esp_random());
}

}  // namespace borges
