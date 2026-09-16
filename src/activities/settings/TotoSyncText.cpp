#include "TotoSyncText.h"

#include <I18n.h>

#include <array>
#include <cstdio>

#include "TotoCredentialStore.h"

namespace toto_ui {

std::string accountSentence() {
  if (!TOTO_CREDENTIALS.paired()) return tr(STR_TOTO_SIGNED_OUT);
  const std::string& account = TOTO_CREDENTIALS.getAccountUsername();
  // A reader that arrived through code pairing never learned the account name.
  if (account.empty()) return tr(STR_TOTO_PAIRED);
  std::array<char, 128> line{};
  std::snprintf(line.data(), line.size(), tr(STR_TOTO_ACCOUNT), account.c_str());
  return line.data();
}

std::string statusSentence(const toto::SyncSnapshot& snapshot) {
  switch (toto::summarize(snapshot)) {
    case toto::StatusKind::SignedOut:
      return tr(STR_TOTO_SIGNED_OUT);
    case toto::StatusKind::PairingPending:
      return tr(STR_TOTO_PAIR_WEB);
    case toto::StatusKind::SessionExpired:
      return tr(STR_TOTO_SESSION_EXPIRED);
    case toto::StatusKind::SessionRenewDue:
      return tr(STR_TOTO_SESSION_RENEW);
    case toto::StatusKind::RemotePositionWaiting:
      return tr(STR_TOTO_STATUS_REMOTE_WAITING);
    case toto::StatusKind::PendingUploads: {
      std::array<char, 64> line{};
      std::snprintf(line.data(), line.size(), tr(STR_TOTO_STATUS_PENDING),
                    static_cast<unsigned>(snapshot.outbox + snapshot.inbox));
      return line.data();
    }
    case toto::StatusKind::UpToDate:
      break;
  }
  return tr(STR_TOTO_STATUS_UP_TO_DATE);
}

std::string lastSyncSentence(const toto::SyncSnapshot& snapshot) {
  // Every branch returns, so no path can reach snprintf with a null format,
  // and a new AgeUnit is a -Wswitch error rather than a silent "days ago".
  const auto counted = [](const char* format, uint32_t value) {
    std::array<char, 64> line{};
    std::snprintf(line.data(), line.size(), format, static_cast<unsigned>(value));
    return std::string(line.data());
  };

  const toto::Age age = toto::lastSuccessAge(snapshot);
  switch (age.unit) {
    case toto::AgeUnit::Never:
      break;
    case toto::AgeUnit::Moments:
      return tr(STR_TOTO_LAST_SYNC_MOMENTS);
    case toto::AgeUnit::Minutes:
      return counted(tr(STR_TOTO_LAST_SYNC_MINUTES), age.value);
    case toto::AgeUnit::Hours:
      return counted(tr(STR_TOTO_LAST_SYNC_HOURS), age.value);
    case toto::AgeUnit::Days:
      return counted(tr(STR_TOTO_LAST_SYNC_DAYS), age.value);
  }
  return tr(STR_TOTO_LAST_SYNC_NEVER);
}

}  // namespace toto_ui
