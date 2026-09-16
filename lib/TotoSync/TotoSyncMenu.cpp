#include "TotoSyncMenu.h"

namespace toto {
namespace {

constexpr uint64_t SECONDS_PER_MINUTE = 60;
constexpr uint64_t SECONDS_PER_HOUR = 60 * SECONDS_PER_MINUTE;
constexpr uint64_t SECONDS_PER_DAY = 24 * SECONDS_PER_HOUR;
// A report code is read off the screen and typed into a web form, so it is
// capped at something a person will actually finish copying.
constexpr size_t MAX_REPORT_CODE_LENGTH = 64;

}  // namespace

StatusKind summarize(const SyncSnapshot& snapshot) {
  if (!snapshot.signedIn) {
    return snapshot.pairingPending ? StatusKind::PairingPending : StatusKind::SignedOut;
  }
  // An expired session is the only thing that makes every other line a lie:
  // the queue is not "waiting to send", it cannot be sent at all.
  if (snapshot.session == SessionState::EXPIRED) return StatusKind::SessionExpired;
  if (snapshot.session == SessionState::RENEW_DUE) return StatusKind::SessionRenewDue;
  if (snapshot.remotePositionWaiting) return StatusKind::RemotePositionWaiting;
  if (snapshot.outbox > 0 || snapshot.inbox > 0) return StatusKind::PendingUploads;
  return StatusKind::UpToDate;
}

Age lastSuccessAge(const SyncSnapshot& snapshot) {
  // now == 0 means the clock was never trusted this boot. A reader that cannot
  // date the sync is told "never" rather than shown arithmetic on garbage.
  if (snapshot.lastSuccessAt == 0 || snapshot.now < snapshot.lastSuccessAt) return {};
  const uint64_t elapsed = snapshot.now - snapshot.lastSuccessAt;
  if (elapsed < SECONDS_PER_MINUTE) return {AgeUnit::Moments, 0};
  if (elapsed < SECONDS_PER_HOUR) return {AgeUnit::Minutes, static_cast<uint32_t>(elapsed / SECONDS_PER_MINUTE)};
  if (elapsed < SECONDS_PER_DAY) return {AgeUnit::Hours, static_cast<uint32_t>(elapsed / SECONDS_PER_HOUR)};
  return {AgeUnit::Days, static_cast<uint32_t>(elapsed / SECONDS_PER_DAY)};
}

bool rowEnabled(MenuRow row, const SyncSnapshot& snapshot) {
  switch (row) {
    case MenuRow::SyncNow:
    case MenuRow::Library:
      // Both of these spend the session. Offering them signed out would only
      // teach the reader that the menu lies.
      return snapshot.signedIn;
    case MenuRow::Account:
    case MenuRow::StatusHelp:
    case MenuRow::Advanced:
      break;
  }
  return true;
}

bool advancedRowEnabled(AdvancedRow row, const SyncSnapshot& snapshot) {
  switch (row) {
    case AdvancedRow::PairWithCode:
      return !snapshot.signedIn;
    case AdvancedRow::RepairServices:
      return snapshot.signedIn;
    case AdvancedRow::DiscardRemotePosition:
      return snapshot.remotePositionWaiting;
  }
  return false;
}

std::string reportCode(std::string_view firmwareVersion, std::string_view deviceId) {
  std::string code(firmwareVersion);
  if (code.empty()) code = "?";
  code += " / ";
  code += deviceId.empty() ? "-" : std::string(deviceId);
  if (code.size() > MAX_REPORT_CODE_LENGTH) code.resize(MAX_REPORT_CODE_LENGTH);
  return code;
}

std::string supportHost(std::string_view baseUrl) {
  constexpr std::string_view HTTPS = "https://";
  if (baseUrl.rfind(HTTPS, 0) == 0) baseUrl.remove_prefix(HTTPS.size());
  const size_t slash = baseUrl.find('/');
  if (slash != std::string_view::npos) baseUrl = baseUrl.substr(0, slash);
  return std::string(baseUrl);
}

}  // namespace toto
