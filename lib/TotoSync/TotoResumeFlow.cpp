#include "TotoResumeFlow.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace toto {
namespace {

constexpr char FIELD_SEPARATOR = '\t';
constexpr char RECORD_SEPARATOR = '\n';
constexpr char LOG_VERSION[] = "v1";

std::string_view trimmed(std::string_view value) {
  size_t begin = 0;
  while (begin < value.size() && static_cast<unsigned char>(value[begin]) <= ' ') ++begin;
  size_t end = value.size();
  while (end > begin && static_cast<unsigned char>(value[end - 1]) <= ' ') --end;
  return value.substr(begin, end - begin);
}

// Tabs and newlines are the record structure, so anything that could carry
// them is flattened before it is written. Nothing legitimate is lost: an
// xpointer and a uuid have neither.
std::string sanitized(std::string_view value) {
  std::string result(value);
  for (char& character : result) {
    if (character == FIELD_SEPARATOR || character == RECORD_SEPARATOR || character == '\r') character = ' ';
  }
  return result;
}

bool looksTechnical(std::string_view value) {
  if (value.empty()) return true;
  for (const char character : value) {
    const bool hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                     (character >= 'A' && character <= 'F');
    if (!hex && character != '-') return false;
  }
  return true;
}

std::string formatFixed(double value, int decimals) {
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
  return buffer.data();
}

std::optional<double> parseDouble(std::string_view text) {
  if (text.empty()) return std::nullopt;
  const std::string owned(text);
  char* end = nullptr;
  const double parsed = std::strtod(owned.c_str(), &end);
  if (end == owned.c_str() || *end != '\0' || !std::isfinite(parsed)) return std::nullopt;
  return parsed;
}

std::optional<uint64_t> parseUnsigned(std::string_view text) {
  if (text.empty()) return std::nullopt;
  uint64_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return parsed;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t begin = 0;
  while (true) {
    const size_t found = text.find(separator, begin);
    if (found == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      break;
    }
    parts.push_back(text.substr(begin, found - begin));
    begin = found + 1;
  }
  return parts;
}

}  // namespace

std::string resumeFingerprint(const ResumePosition& position) {
  const std::string_view eventId = trimmed(position.eventId);
  if (!eventId.empty()) return "event:" + std::string(eventId);
  if (position.serverSequence != 0) return "seq:" + std::to_string(position.serverSequence);

  std::string locator;
  const std::string_view xpointer = trimmed(position.xpointer);
  if (!xpointer.empty()) locator = "xp=" + std::string(xpointer);
  if (position.currentPage != 0) {
    if (!locator.empty()) locator.push_back('&');
    locator += "pg=" + std::to_string(position.currentPage);
  }
  if (position.hasPercentage) {
    if (!locator.empty()) locator.push_back('&');
    locator += "pct=" + formatFixed(position.percentage, 2);
  }
  if (locator.empty()) return {};
  return "pos:" + locator;
}

bool resumeHasLocator(const ResumePosition& position) {
  if (!trimmed(position.xpointer).empty()) return true;
  if (position.currentPage != 0) return true;
  return position.hasPercentage && position.percentage >= 0;
}

std::optional<double> resumeComparablePercentage(const ResumePosition& position, uint32_t totalPagesHint) {
  if (position.hasPercentage) return position.percentage;
  const uint32_t total = position.totalPages != 0 ? position.totalPages : totalPagesHint;
  if (position.currentPage == 0 || total == 0) return std::nullopt;
  return (static_cast<double>(position.currentPage) / static_cast<double>(total)) * 100.0;
}

bool ConnectPlan::includes(ConnectStep step) const {
  for (uint8_t index = 0; index < stepCount; ++index) {
    if (steps[index] == step) return true;
  }
  return false;
}

bool ConnectPlan::pullsBeforeDrain() const {
  if (!includes(ConnectStep::Pull) || !includes(ConnectStep::Drain)) return false;
  return steps[0] == ConnectStep::Pull;
}

ConnectPlan planConnect(const ConnectState& state) {
  ConnectPlan plan;
  plan.stepCount = 0;
  if (state.running) {
    plan.skip = ConnectSkip::Running;
    return plan;
  }
  if (!state.connected) {
    plan.skip = ConnectSkip::Offline;
    return plan;
  }
  if (!state.hasSession) {
    plan.skip = ConnectSkip::NoSession;
    return plan;
  }
  if (!state.force && state.hasLastSync && state.minInterval > 0 &&
      static_cast<uint32_t>(state.now - state.lastSyncAt) < state.minInterval) {
    plan.skip = ConnectSkip::Debounced;
    return plan;
  }

  if (state.autoPull) plan.steps[plan.stepCount++] = ConnectStep::Pull;
  plan.steps[plan.stepCount++] = ConnectStep::Drain;
  return plan;
}

ResumeDecision* ResumeDecisionLog::mutableFind(std::string_view bookHash) {
  for (Entry& entry : log) {
    if (entry.bookHash == bookHash) return &entry.decision;
  }
  return nullptr;
}

const ResumeDecision* ResumeDecisionLog::find(std::string_view bookHash) const {
  for (const Entry& entry : log) {
    if (entry.bookHash == bookHash) return &entry.decision;
  }
  return nullptr;
}

void ResumeDecisionLog::dropOldest() {
  if (log.empty()) return;
  size_t oldest = 0;
  for (size_t index = 1; index < log.size(); ++index) {
    if (log[index].decision.at < log[oldest].decision.at) oldest = index;
  }
  log.erase(log.begin() + static_cast<std::ptrdiff_t>(oldest));
}

bool ResumeDecisionLog::remember(std::string_view bookHash, const ResumePosition& position, bool accepted,
                                 std::string_view suggestionId, uint64_t at, bool synced) {
  if (trimmed(bookHash).empty()) return false;
  const std::string fingerprint = resumeFingerprint(position);
  if (fingerprint.empty()) return false;

  ResumeDecision decision;
  decision.fingerprint = fingerprint;
  decision.accepted = accepted;
  if (const auto percentage = resumeComparablePercentage(position)) {
    decision.hasPercentage = true;
    decision.percentage = *percentage;
  }
  decision.at = at;
  decision.suggestionId = std::string(trimmed(suggestionId));
  decision.synced = synced || decision.suggestionId.empty();

  if (ResumeDecision* existing = mutableFind(bookHash)) {
    *existing = decision;
    return true;
  }
  if (log.size() >= RESUME_MAX_BOOKS) dropOldest();
  log.push_back(Entry{std::string(bookHash), std::move(decision)});
  return true;
}

bool ResumeDecisionLog::forget(std::string_view bookHash) {
  for (size_t index = 0; index < log.size(); ++index) {
    if (log[index].bookHash != bookHash) continue;
    log.erase(log.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }
  return false;
}

void ResumeDecisionLog::clear() { log.clear(); }

bool ResumeDecisionLog::isResolved(std::string_view bookHash, const ResumePosition& position) const {
  const ResumeDecision* decision = find(bookHash);
  if (decision == nullptr) return false;
  const std::string fingerprint = resumeFingerprint(position);
  if (!fingerprint.empty() && decision->fingerprint == fingerprint) return true;

  if (!decision->hasPercentage) return false;
  const auto candidate = resumeComparablePercentage(position);
  if (!candidate) return false;
  return std::fabs(*candidate - decision->percentage) <= RESUME_EQUIVALENT_PCT;
}

std::vector<ResumeDecisionLog::Entry> ResumeDecisionLog::pendingResolutions() const {
  std::vector<Entry> pending;
  for (const Entry& entry : log) {
    if (entry.decision.synced || entry.decision.suggestionId.empty()) continue;
    pending.push_back(entry);
  }
  std::stable_sort(pending.begin(), pending.end(),
                   [](const Entry& left, const Entry& right) { return left.decision.at < right.decision.at; });
  return pending;
}

bool ResumeDecisionLog::markSynced(std::string_view bookHash, std::string_view suggestionId) {
  ResumeDecision* decision = mutableFind(bookHash);
  if (decision == nullptr || decision->suggestionId != suggestionId) return false;
  decision->synced = true;
  return true;
}

std::string ResumeDecisionLog::serialize() const {
  std::string text = LOG_VERSION;
  text.push_back(RECORD_SEPARATOR);
  for (const Entry& entry : log) {
    text += sanitized(entry.bookHash);
    text.push_back(FIELD_SEPARATOR);
    text += sanitized(entry.decision.fingerprint);
    text.push_back(FIELD_SEPARATOR);
    text.push_back(entry.decision.accepted ? 'a' : 'd');
    text.push_back(FIELD_SEPARATOR);
    if (entry.decision.hasPercentage) text += formatFixed(entry.decision.percentage, 2);
    text.push_back(FIELD_SEPARATOR);
    text += std::to_string(entry.decision.at);
    text.push_back(FIELD_SEPARATOR);
    text += sanitized(entry.decision.suggestionId);
    text.push_back(FIELD_SEPARATOR);
    text.push_back(entry.decision.synced ? '1' : '0');
    text.push_back(RECORD_SEPARATOR);
  }
  return text;
}

ResumeDecisionLog ResumeDecisionLog::parse(std::string_view text) {
  ResumeDecisionLog result;
  const std::vector<std::string_view> lines = split(text, RECORD_SEPARATOR);
  if (lines.empty() || trimmed(lines[0]) != LOG_VERSION) return result;

  for (size_t index = 1; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    if (trimmed(line).empty()) continue;
    const std::vector<std::string_view> fields = split(line, FIELD_SEPARATOR);
    if (fields.size() < 7) continue;
    const std::string_view bookHash = trimmed(fields[0]);
    const std::string_view fingerprint = trimmed(fields[1]);
    if (bookHash.empty() || fingerprint.empty()) continue;

    ResumeDecision decision;
    decision.fingerprint = std::string(fingerprint);
    decision.accepted = trimmed(fields[2]) == "a";
    if (const auto percentage = parseDouble(trimmed(fields[3]))) {
      decision.hasPercentage = true;
      decision.percentage = *percentage;
    }
    decision.at = parseUnsigned(trimmed(fields[4])).value_or(0);
    decision.suggestionId = std::string(trimmed(fields[5]));
    decision.synced = trimmed(fields[6]) != "0" || decision.suggestionId.empty();

    if (result.log.size() >= RESUME_MAX_BOOKS) result.dropOldest();
    result.log.push_back(Entry{std::string(bookHash), std::move(decision)});
  }
  return result;
}

OfferRefusal shouldOffer(const ResumeDecisionLog& decisions, const OfferContext& context) {
  if (trimmed(context.bookHash).empty()) return OfferRefusal::NoBook;
  // The book can change between the event arriving and the question being
  // asked. A popup about another book is worse than no warning at all.
  if (trimmed(context.openBookHash).empty()) return OfferRefusal::NoBook;
  if (context.openBookHash != context.bookHash) return OfferRefusal::OtherBook;
  // Same with the account: a suggestion issued for the previous owner is not
  // shown to whoever signed in afterwards.
  if (!context.accountScope.empty() && !context.currentAccount.empty() &&
      context.accountScope != context.currentAccount) {
    return OfferRefusal::OtherAccount;
  }
  if (!resumeHasLocator(context.remote)) return OfferRefusal::NoLocator;
  if (context.dialogVisible) return OfferRefusal::AlreadyVisible;

  if (context.hasLocal) {
    const auto remote = resumeComparablePercentage(context.remote, context.totalPages);
    const auto here = resumeComparablePercentage(context.local, context.totalPages);
    if (remote && here && std::fabs(*remote - *here) <= RESUME_EQUIVALENT_PCT) return OfferRefusal::SamePosition;
  }
  if (decisions.isResolved(context.bookHash, context.remote)) return OfferRefusal::AlreadyResolved;
  return OfferRefusal::None;
}

const char* offerRefusalName(OfferRefusal refusal) {
  switch (refusal) {
    case OfferRefusal::None:
      return "offer";
    case OfferRefusal::NoBook:
      return "no_book";
    case OfferRefusal::OtherBook:
      return "other_book";
    case OfferRefusal::OtherAccount:
      return "other_account";
    case OfferRefusal::AlreadyVisible:
      return "already_visible";
    case OfferRefusal::SamePosition:
      return "same_position";
    case OfferRefusal::AlreadyResolved:
      return "already_resolved";
    case OfferRefusal::NoLocator:
      return "no_locator";
  }
  return "unknown";
}

PositionSummary summarizePosition(const ResumePosition& position, uint32_t totalPagesHint) {
  PositionSummary summary;
  if (position.currentPage != 0) {
    summary.hasPage = true;
    summary.page = position.currentPage;
    summary.totalPages = position.totalPages != 0 ? position.totalPages : totalPagesHint;
  }
  if (const auto percentage = resumeComparablePercentage(position, totalPagesHint)) {
    summary.hasPercentage = true;
    summary.percentage = std::clamp(*percentage, 0.0, 100.0);
  }
  summary.known = summary.hasPage || summary.hasPercentage;
  return summary;
}

std::string resumeSourceName(std::string_view name, std::string_view platform) {
  const std::string_view trimmedName = trimmed(name);
  if (!trimmedName.empty() && !looksTechnical(trimmedName)) return std::string(trimmedName);
  const std::string_view trimmedPlatform = trimmed(platform);
  if (!trimmedPlatform.empty() && !looksTechnical(trimmedPlatform)) return std::string(trimmedPlatform);
  return {};
}

bool resumeTrustedTimePrecision(std::string_view precision) {
  std::string lowered(trimmed(precision));
  for (char& character : lowered) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return lowered == "exact" || lowered == "anchored";
}

ResumeCard describeResume(const DescribeContext& context) {
  ResumeCard card;
  card.there = summarizePosition(context.remote, context.totalPages);
  if (context.hasLocal) card.here = summarizePosition(context.local, context.totalPages);
  card.originName = resumeSourceName(context.originName, context.originPlatform);

  const auto there = resumeComparablePercentage(context.remote, context.totalPages);
  const auto here = context.hasLocal ? resumeComparablePercentage(context.local, context.totalPages) : std::nullopt;
  if (there && here) {
    const double delta = *there - *here;
    if (delta < -RESUME_EQUIVALENT_PCT) {
      card.backwards = true;
    } else if (std::fabs(delta) <= RESUME_SMALL_JUMP_PCT) {
      card.smallJump = true;
    }
  }

  // Without an anchor the destination is computed from the percentage, which
  // lands a few lines off. The reader deserves to know that before jumping.
  card.approximate = trimmed(context.remote.xpointer).empty();
  card.trustedTime = context.hasOccurredAt && resumeTrustedTimePrecision(context.timePrecision);
  return card;
}

}  // namespace toto
