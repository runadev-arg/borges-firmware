#pragma once

#include <cstdint>

namespace borges {

class SyncScheduler {
 public:
  static SyncScheduler& instance();
  void notifyLifecycleCommit();
  void tick(bool readerActive);
  bool readerSyncDue(bool pageSettled, bool buildActive);
  void syncNow(uint8_t maxExchanges = 1);
  void flushBeforeSleep();

 private:
  SyncScheduler() = default;

  void refreshConnectivitySchedule();
  void runBurst(uint8_t maxExchanges);

  uint32_t nextAttemptMs = 0;
  uint32_t lastSuccessMs = 0;
  uint8_t failures = 0;
  bool pending = false;
  bool running = false;
  bool wifiWasConnected = false;
};

}  // namespace borges

#define BORGES_SYNC_SCHEDULER borges::SyncScheduler::instance()
