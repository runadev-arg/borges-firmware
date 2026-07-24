#include "TotoTrust.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "TotoDurableQueue.h"

namespace toto {

bool ensureTrustedClock() {
  constexpr time_t MIN_TRUSTED_TIME = 1767225600;  // 2026-01-01
  if (time(nullptr) >= MIN_TRUSTED_TIME) return true;

  if (TOTO_QUEUE.begin() && TOTO_QUEUE.checkpoint().wallAnchorUnixSeconds >= static_cast<uint64_t>(MIN_TRUSTED_TIME)) {
    const timeval approximate{
        .tv_sec = static_cast<time_t>(TOTO_QUEUE.checkpoint().wallAnchorUnixSeconds),
        .tv_usec = 0,
    };
    settimeofday(&approximate, nullptr);
  }

  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  const uint32_t deadline = millis() + 10000U;
  while (time(nullptr) < MIN_TRUSTED_TIME && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  return time(nullptr) >= MIN_TRUSTED_TIME;
}

}  // namespace toto
