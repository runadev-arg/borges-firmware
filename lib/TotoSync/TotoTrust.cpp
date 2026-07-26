#include "TotoTrust.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoNetBoot.h"

namespace toto {

bool ensureTrustedClock(std::string* detail) {
  if (time(nullptr) >= netboot::MIN_TRUSTED_TIME) return true;

  // The checkpoint anchor already satisfies the trust window, so the SNTP wait
  // below exits on its first check. Remembering that keeps the reported tag
  // honest: claiming "sntp:ok" on a network where SNTP never answered is
  // exactly the wrong thing to tell whoever is debugging it in the field.
  bool fromAnchor = false;
  if (TOTO_QUEUE.begin() &&
      TOTO_QUEUE.checkpoint().wallAnchorUnixSeconds >= static_cast<uint64_t>(netboot::MIN_TRUSTED_TIME)) {
    const timeval approximate{
        .tv_sec = static_cast<time_t>(TOTO_QUEUE.checkpoint().wallAnchorUnixSeconds),
        .tv_usec = 0,
    };
    settimeofday(&approximate, nullptr);
    fromAnchor = true;
  }

  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  const uint32_t deadline = millis() + 10000U;
  while (time(nullptr) < netboot::MIN_TRUSTED_TIME && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  if (time(nullptr) >= netboot::MIN_TRUSTED_TIME) {
    if (detail != nullptr) *detail = fromAnchor ? "anchor:ok" : "sntp:ok";
    return true;
  }

  // SNTP is DNS plus UDP, and on this network both are dead. The Date header of
  // an HTTP response travels over plain TCP and is enough to validate certs.
  // The security exposure is the same as clear-text NTP (a MITM can move the
  // clock): bounded by the sanity window above and below, and by the fact that
  // the value only enables verification of a certificate that still has to
  // chain to the pinned root.
  //
  // esp_sntp_stop() before touching the clock, so a late sync cannot overwrite
  // it afterwards. (The plan says sntp_stop(); the esp_ wrapper is used instead
  // because it takes the lwIP lock, which is precisely the class of bug this
  // whole change exists to avoid.)
  esp_sntp_stop();

  const std::string host = netboot::hostFromBaseUrl(TOTO_CREDENTIALS.getBaseUrl());
  if (host.empty()) {
    if (detail != nullptr) *detail = "httpdate:fail(url)";
    return false;
  }
  const netboot::Candidates candidates = netboot::resolveServer(host.c_str());  // memoized
  std::string dateDetail = "httpdate:fail(tcp)";
  for (uint8_t index = 0; index < candidates.count; ++index) {
    const uint64_t epoch = netboot::httpDateEpoch(candidates.ip[index], host.c_str(), dateDetail);
    if (epoch == 0) continue;
    const timeval now{.tv_sec = static_cast<time_t>(epoch), .tv_usec = 0};
    settimeofday(&now, nullptr);
    if (detail != nullptr) *detail = candidates.detail + " " + dateDetail;
    LOG_INF("TOTO", "Clock bootstrapped from HTTP Date header");
    return true;
  }

  // Port 80 gave nothing usable. Carrier-filtered hotspots have been seen
  // passing ONLY 443 (port 53 blocked even over TCP, port 80 intercepted
  // without a response), so the Date is read once more over TLS. See
  // httpsDateEpoch for why skipping verification is sound for this one read.
  std::string httpsDetail = "httpsdate:fail(tcp)";
  for (uint8_t index = 0; index < candidates.count; ++index) {
    const uint64_t epoch = netboot::httpsDateEpoch(candidates.ip[index], host.c_str(), httpsDetail);
    if (epoch == 0) continue;
    const timeval now{.tv_sec = static_cast<time_t>(epoch), .tv_usec = 0};
    settimeofday(&now, nullptr);
    if (detail != nullptr) *detail = candidates.detail + " " + dateDetail + " " + httpsDetail;
    LOG_INF("TOTO", "Clock bootstrapped from HTTPS Date header");
    return true;
  }
  if (detail != nullptr) *detail = candidates.detail + " " + dateDetail + " " + httpsDetail;
  return false;
}

}  // namespace toto
