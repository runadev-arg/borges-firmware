#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Picking the reading position back up after another device moved it.
//
// This is the CrossPoint side of the same contract the KOReader plugin calls
// C17, and it keeps its three decisions:
//
//   * planConnect  -- on reconnect the reader asks before it sends. Draining
//                     first makes the hub take this reader's stale offline
//                     position as the newest write, and the position the other
//                     device left stops being offered at all.
//   * shouldOffer  -- one question on screen at a time, for the open book and
//                     the account that is signed in now. What was already
//                     answered is not asked again; a genuinely new position is.
//   * describe     -- what the reader compares before deciding: where they are
//                     here, where the other device left them, and the two
//                     warnings that change the answer.
//
// What it deliberately does NOT do is pick a winner. Arrival order, a higher
// percentage and the clock of a reader that spent a month in a drawer prove
// nothing. Both positions are shown and the person decides.
//
// No Arduino, no I18n, no Storage on purpose: that is what makes these rules
// testable on the host, where no reader exists. The activities read the live
// singletons, hand a snapshot over, and translate whatever comes back.
namespace toto {

// Two positions closer than this are the same position: the other device
// closed the book half a line further on and that is not worth a dialog.
inline constexpr double RESUME_EQUIVALENT_PCT = 0.5;

// Below this distance the jump is "a small difference" and is described that
// way, instead of announcing a trip that does not happen.
inline constexpr double RESUME_SMALL_JUMP_PCT = 2.0;

// How many books remember their last answer. A reader with hundreds of books
// does not have to carry hundreds of dead decisions on a 380 KB device.
inline constexpr size_t RESUME_MAX_BOOKS = 40;

// A position as it travels: what the hub sent, or what this reader is showing.
struct ResumePosition {
  bool hasPercentage = false;
  double percentage = 0;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  std::string xpointer;
  // Identity the hub numbered. Preferred over the locator because it is what
  // tells two writes apart without trusting anybody's clock.
  std::string eventId;
  uint64_t serverSequence = 0;
};

// Stable identity of a proposed position.
//
// First what the hub numbers -- event id, then server sequence. Only when
// neither came does it fall back to the locator, rounded: a percentage with
// fourteen decimals would look like a new position on every pull.
std::string resumeFingerprint(const ResumePosition& position);

// Is there anywhere to go? Without an anchor, a page or a percentage there is
// no destination, and offering it would promise a jump that later fails.
bool resumeHasLocator(const ResumePosition& position);

// Percentage usable for comparison, computed from pages when the percentage
// did not come. Only ever used to compare, never to navigate.
std::optional<double> resumeComparablePercentage(const ResumePosition& position, uint32_t totalPagesHint = 0);

// ---------------------------------------------------------------------------
// The order of a reconnection
// ---------------------------------------------------------------------------

enum class ConnectStep : uint8_t { Pull, Drain };

enum class ConnectSkip : uint8_t { None, Running, Offline, NoSession, Debounced };

struct ConnectState {
  bool connected = false;
  bool hasSession = false;
  bool running = false;
  bool autoPull = true;
  bool force = false;
  // Milliseconds, on the reader's monotonic clock.
  uint32_t now = 0;
  uint32_t lastSyncAt = 0;
  uint32_t minInterval = 0;
  bool hasLastSync = false;
};

struct ConnectPlan {
  ConnectStep steps[2] = {ConnectStep::Pull, ConnectStep::Drain};
  uint8_t stepCount = 0;
  ConnectSkip skip = ConnectSkip::None;

  bool includes(ConnectStep step) const;
  // The pull has to be the first step whenever both run: that ordering is the
  // whole point, so it is worth being able to assert it.
  bool pullsBeforeDrain() const;
};

// Unlike KOReader, CrossPoint's pull is not per book -- it also carries
// annotations and library changes -- so no book has to be open for it to be
// worth doing. What survives unchanged is the order, and that a pull which
// failed cancels the drain instead of "making the most of having signal".
ConnectPlan planConnect(const ConnectState& state);

// ---------------------------------------------------------------------------
// The memory of what was already answered
// ---------------------------------------------------------------------------

struct ResumeDecision {
  std::string fingerprint;
  bool accepted = false;
  bool hasPercentage = false;
  double percentage = 0;
  uint64_t at = 0;
  // The hub's id for the suggestion, kept so the answer can still be delivered
  // when it was given with no signal, and `synced` once the hub took it.
  std::string suggestionId;
  bool synced = true;
};

// book hash -> the last answer given for that book.
class ResumeDecisionLog {
 public:
  struct Entry {
    std::string bookHash;
    ResumeDecision decision;
  };

  // Returns false when the book hash or the position carries no identity to
  // remember, which is the only way a decision can fail to be recorded.
  bool remember(std::string_view bookHash, const ResumePosition& position, bool accepted, std::string_view suggestionId,
                uint64_t at, bool synced);
  const ResumeDecision* find(std::string_view bookHash) const;
  bool forget(std::string_view bookHash);
  void clear();

  // Already answered? "The same" means the exact fingerprint, or a position
  // that did not move from the answered one: reconnecting ten times cannot
  // turn into ten identical questions. A really new position is asked again
  // even when the previous one was turned down.
  bool isResolved(std::string_view bookHash, const ResumePosition& position) const;

  // Answers the hub has not been told about yet, oldest first.
  std::vector<Entry> pendingResolutions() const;
  bool markSynced(std::string_view bookHash, std::string_view suggestionId);

  const std::vector<Entry>& entries() const { return log; }
  size_t size() const { return log.size(); }

  // Flat text, one line per book: no JSON document has to be held in RAM to
  // read four fields back.
  std::string serialize() const;
  static ResumeDecisionLog parse(std::string_view text);

 private:
  std::vector<Entry> log;

  ResumeDecision* mutableFind(std::string_view bookHash);
  void dropOldest();
};

// ---------------------------------------------------------------------------
// The gate: one question at a time
// ---------------------------------------------------------------------------

// Why a position is not being offered. These are part of the contract -- the
// tests and the log use them -- so they only get renamed together with a test.
enum class OfferRefusal : uint8_t {
  None,
  NoBook,
  OtherBook,
  OtherAccount,
  AlreadyVisible,
  SamePosition,
  AlreadyResolved,
  NoLocator,
};

struct OfferContext {
  std::string bookHash;
  std::string openBookHash;
  // Account the suggestion was issued for, and the one signed in now. Empty
  // means "not known", and an unknown scope never blocks the question.
  std::string accountScope;
  std::string currentAccount;
  ResumePosition remote;
  ResumePosition local;
  bool hasLocal = false;
  uint32_t totalPages = 0;
  bool dialogVisible = false;
};

OfferRefusal shouldOffer(const ResumeDecisionLog& decisions, const OfferContext& context);
const char* offerRefusalName(OfferRefusal refusal);

// ---------------------------------------------------------------------------
// What ends up on screen
// ---------------------------------------------------------------------------

// One side of the comparison, already reduced to what the sentence needs. The
// xpointer and the event id never reach it: those live in the detail line.
struct PositionSummary {
  bool known = false;
  bool hasPage = false;
  uint32_t page = 0;
  uint32_t totalPages = 0;
  bool hasPercentage = false;
  double percentage = 0;
};

PositionSummary summarizePosition(const ResumePosition& position, uint32_t totalPagesHint = 0);

struct DescribeContext {
  ResumePosition remote;
  ResumePosition local;
  bool hasLocal = false;
  uint32_t totalPages = 0;
  std::string originName;
  std::string originPlatform;
  std::string timePrecision;
  bool hasOccurredAt = false;
};

struct ResumeCard {
  PositionSummary here;
  PositionSummary there;
  // Empty when nothing readable came: the screen then says "another device".
  std::string originName;
  bool backwards = false;
  bool smallJump = false;
  bool approximate = false;
  // The date is only shown when the origin clock is trustworthy. A reader that
  // spent a month without battery reports an hour that never happened.
  bool trustedTime = false;
};

ResumeCard describeResume(const DescribeContext& context);

// Presentable name of the other reader. A technical id is not a name: when
// nothing readable is left this returns empty and the caller says so.
std::string resumeSourceName(std::string_view name, std::string_view platform);

bool resumeTrustedTimePrecision(std::string_view precision);

}  // namespace toto
