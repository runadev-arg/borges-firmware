#pragma once

#include <cstdint>

namespace toto {

class SyncScheduler {
 public:
  static SyncScheduler& instance();
  void notifyLifecycleCommit();
  void tick(bool readerActive);

  // Bumped by every successful sync. Screens watch it instead of listing the
  // inbox on every loop tick, which on this device is an SD directory read.
  uint32_t syncGeneration() const { return completedSyncs; }

 private:
  SyncScheduler() = default;

  uint32_t nextAttemptMs = 0;
  uint32_t lastSuccessMs = 0;
  uint32_t completedSyncs = 0;
  uint8_t failures = 0;
  bool pending = false;
  bool running = false;
  bool wifiWasConnected = false;
  // Set the moment Wi-Fi comes back: the first request of a reconnection asks
  // without sending. Cleared only by a pull that actually succeeded, so a
  // failed pull cannot be followed by a drain that buries the answer.
  bool askBeforeSending = false;
};

}  // namespace toto

#define TOTO_SYNC_SCHEDULER toto::SyncScheduler::instance()
