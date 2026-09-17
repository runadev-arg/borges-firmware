#include "TotoSyncText.h"

#include <I18n.h>

#include <array>
#include <cstdio>
#include <utility>

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

std::string positionSentence(const toto::PositionSummary& summary) {
  if (!summary.known) return tr(STR_TOTO_RESUME_UNKNOWN);

  std::string sentence;
  std::array<char, 64> chunk{};
  if (summary.hasPage) {
    if (summary.totalPages > 0) {
      std::snprintf(chunk.data(), chunk.size(), tr(STR_TOTO_RESUME_PAGE_OF), static_cast<unsigned>(summary.page),
                    static_cast<unsigned>(summary.totalPages));
    } else {
      std::snprintf(chunk.data(), chunk.size(), tr(STR_TOTO_RESUME_PAGE), static_cast<unsigned>(summary.page));
    }
    sentence = chunk.data();
  }
  if (summary.hasPercentage) {
    std::snprintf(chunk.data(), chunk.size(), tr(STR_TOTO_RESUME_PERCENT),
                  static_cast<unsigned>(summary.percentage + 0.5));
    if (sentence.empty()) {
      sentence = chunk.data();
    } else {
      sentence += " - ";
      sentence += chunk.data();
    }
  }
  return sentence;
}

std::vector<std::string> resumeCardLines(const toto::ResumeCard& card) {
  std::array<char, 160> line{};
  std::vector<std::string> lines;

  std::snprintf(line.data(), line.size(), tr(STR_TOTO_RESUME_HERE), positionSentence(card.here).c_str());
  lines.emplace_back(line.data());

  const std::string origin = card.originName.empty() ? std::string(tr(STR_TOTO_RESUME_OTHER_DEVICE)) : card.originName;
  std::snprintf(line.data(), line.size(), tr(STR_TOTO_RESUME_THERE), origin.c_str(),
                positionSentence(card.there).c_str());
  lines.emplace_back(line.data());

  // The two warnings that change the answer go together, after a blank line
  // and before the options: a reader about to travel backwards in their own
  // book should read that before their thumb finds the button.
  std::vector<std::string> warnings;
  if (card.backwards) warnings.emplace_back(tr(STR_TOTO_RESUME_BACKWARDS));
  if (card.smallJump) warnings.emplace_back(tr(STR_TOTO_RESUME_SMALL));
  if (card.approximate) warnings.emplace_back(tr(STR_TOTO_RESUME_APPROXIMATE));
  if (!warnings.empty()) {
    lines.emplace_back();
    for (std::string& warning : warnings) lines.push_back(std::move(warning));
  }
  return lines;
}

std::string resumeHeading() { return tr(STR_TOTO_RESUME_HEADING); }

std::string resumeGoLabel() { return tr(STR_TOTO_RESUME_GO); }

std::string resumeStayLabel() { return tr(STR_TOTO_RESUME_STAY); }

}  // namespace toto_ui
