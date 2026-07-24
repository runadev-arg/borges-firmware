#pragma once

#include <cstdint>

namespace toto {

class SyncScheduler {
 public:
  static SyncScheduler& instance();
  void notifyLifecycleCommit();
  void tick(bool readerActive);

 private:
  SyncScheduler() = default;

  uint32_t nextAttemptMs = 0;
  uint32_t lastSuccessMs = 0;
  uint8_t failures = 0;
  bool pending = false;
  bool running = false;
  bool wifiWasConnected = false;
};

}  // namespace toto

#define TOTO_SYNC_SCHEDULER toto::SyncScheduler::instance()
