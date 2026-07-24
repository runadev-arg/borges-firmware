#include "TotoSyncScheduler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>

#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
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
    nextAttemptMs = millis() + CONNECT_DELAY_MS;
  } else if (wifiConnected && !pending && lastSuccessMs != 0 &&
             static_cast<uint32_t>(millis() - lastSuccessMs) >= IDLE_POLL_INTERVAL_MS) {
    pending = true;
    nextAttemptMs = millis();
  }
  wifiWasConnected = wifiConnected;

  if (running || readerActive || !pending || !TOTO_CREDENTIALS.paired() || !wifiConnected ||
      static_cast<int32_t>(millis() - nextAttemptMs) < 0) {
    return;
  }

  running = true;
  const SyncClient::Outcome outcome = SyncClient::syncOnce();
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

}  // namespace toto
