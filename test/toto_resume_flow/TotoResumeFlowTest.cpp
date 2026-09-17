#include <gtest/gtest.h>

#include "TotoResumeFlow.h"

namespace {

toto::ResumePosition at(double percentage, uint32_t page = 0, uint32_t totalPages = 0,
                        const std::string& xpointer = {}) {
  toto::ResumePosition position;
  position.hasPercentage = true;
  position.percentage = percentage;
  position.currentPage = page;
  position.totalPages = totalPages;
  position.xpointer = xpointer;
  return position;
}

toto::OfferContext offerOf(const toto::ResumePosition& remote, const toto::ResumePosition& local) {
  toto::OfferContext context;
  context.bookHash = "b1";
  context.openBookHash = "b1";
  context.remote = remote;
  context.local = local;
  context.hasLocal = true;
  return context;
}

toto::ConnectState online() {
  toto::ConnectState state;
  state.connected = true;
  state.hasSession = true;
  return state;
}

// ---------------------------------------------------------------------------
// Identity of a position
// ---------------------------------------------------------------------------

TEST(ResumeFingerprint, PrefersWhatTheHubNumbered) {
  toto::ResumePosition position = at(40.0, 12, 200, "/body/p[3]");
  position.eventId = "0f8b1a4c-1111-2222-3333-444455556666";
  position.serverSequence = 91;
  EXPECT_EQ(toto::resumeFingerprint(position), "event:0f8b1a4c-1111-2222-3333-444455556666");

  position.eventId.clear();
  EXPECT_EQ(toto::resumeFingerprint(position), "seq:91");
}

TEST(ResumeFingerprint, FallsBackToARoundedLocator) {
  // Fourteen decimals of the same position must not look like a new one on
  // every pull, which is what the rounding is for.
  const std::string first = toto::resumeFingerprint(at(47.123456789, 12, 200, "/body/p[3]"));
  const std::string second = toto::resumeFingerprint(at(47.124000000, 12, 200, "/body/p[3]"));
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, "pos:xp=/body/p[3]&pg=12&pct=47.12");
}

TEST(ResumeFingerprint, EmptyWhenThereIsNothingToIdentify) {
  EXPECT_TRUE(toto::resumeFingerprint(toto::ResumePosition{}).empty());
}

TEST(ResumeLocator, NeedsSomewhereToGo) {
  EXPECT_FALSE(toto::resumeHasLocator(toto::ResumePosition{}));
  EXPECT_TRUE(toto::resumeHasLocator(at(0.0)));
  toto::ResumePosition onlyAnchor;
  onlyAnchor.xpointer = "/body/p[1]";
  EXPECT_TRUE(toto::resumeHasLocator(onlyAnchor));
}

TEST(ResumeComparable, ComputesPercentageFromPagesWhenItDidNotCome) {
  toto::ResumePosition position;
  position.currentPage = 50;
  position.totalPages = 200;
  const auto percentage = toto::resumeComparablePercentage(position);
  ASSERT_TRUE(percentage.has_value());
  EXPECT_NEAR(*percentage, 25.0, 1e-9);

  position.totalPages = 0;
  EXPECT_FALSE(toto::resumeComparablePercentage(position).has_value());
  EXPECT_TRUE(toto::resumeComparablePercentage(position, 400).has_value());
}

// ---------------------------------------------------------------------------
// The order of a reconnection
// ---------------------------------------------------------------------------

TEST(PlanConnect, AsksBeforeItSends) {
  const toto::ConnectPlan plan = toto::planConnect(online());
  EXPECT_EQ(plan.skip, toto::ConnectSkip::None);
  ASSERT_EQ(plan.stepCount, 2);
  EXPECT_EQ(plan.steps[0], toto::ConnectStep::Pull);
  EXPECT_EQ(plan.steps[1], toto::ConnectStep::Drain);
  EXPECT_TRUE(plan.pullsBeforeDrain());
}

TEST(PlanConnect, StillDrainsWhenThePullIsTurnedOff) {
  toto::ConnectState state = online();
  state.autoPull = false;
  const toto::ConnectPlan plan = toto::planConnect(state);
  ASSERT_EQ(plan.stepCount, 1);
  EXPECT_EQ(plan.steps[0], toto::ConnectStep::Drain);
  EXPECT_FALSE(plan.pullsBeforeDrain());
}

TEST(PlanConnect, SkipsWithAReasonInsteadOfDoingHalfTheWork) {
  toto::ConnectState state = online();
  state.running = true;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::Running);

  state = online();
  state.connected = false;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::Offline);

  state = online();
  state.hasSession = false;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::NoSession);

  for (const toto::ConnectSkip skip :
       {toto::ConnectSkip::Running, toto::ConnectSkip::Offline, toto::ConnectSkip::NoSession}) {
    (void)skip;
  }
}

TEST(PlanConnect, DebouncesButNeverAgainstAnExplicitRequest) {
  toto::ConnectState state = online();
  state.hasLastSync = true;
  state.lastSyncAt = 1000;
  state.now = 3000;
  state.minInterval = 5000;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::Debounced);

  state.force = true;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::None);

  state.force = false;
  state.now = 6001;
  EXPECT_EQ(toto::planConnect(state).skip, toto::ConnectSkip::None);
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

TEST(ShouldOffer, OffersAJumpForwardOnTheOpenBook) {
  const toto::ResumeDecisionLog decisions;
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(60.0), at(30.0))), toto::OfferRefusal::None);
}

TEST(ShouldOffer, NeverForAnotherBookOrWithNoBookOpen) {
  const toto::ResumeDecisionLog decisions;
  toto::OfferContext context = offerOf(at(60.0), at(30.0));
  context.openBookHash = "b2";
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::OtherBook);

  context.openBookHash.clear();
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::NoBook);

  context = offerOf(at(60.0), at(30.0));
  context.bookHash.clear();
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::NoBook);
}

TEST(ShouldOffer, NeverForTheAccountThatSignedOut) {
  const toto::ResumeDecisionLog decisions;
  toto::OfferContext context = offerOf(at(60.0), at(30.0));
  context.accountScope = "ana";
  context.currentAccount = "bruno";
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::OtherAccount);

  // An unknown scope is not evidence of a mismatch, so it does not block.
  context.accountScope.clear();
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::None);
}

TEST(ShouldOffer, OneQuestionOnScreenAtATime) {
  const toto::ResumeDecisionLog decisions;
  toto::OfferContext context = offerOf(at(60.0), at(30.0));
  context.dialogVisible = true;
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::AlreadyVisible);
}

TEST(ShouldOffer, HalfALineFurtherOnIsTheSamePosition) {
  const toto::ResumeDecisionLog decisions;
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(30.4), at(30.0))), toto::OfferRefusal::SamePosition);
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(30.6), at(30.0))), toto::OfferRefusal::None);
}

TEST(ShouldOffer, RefusesWithoutSomewhereToGo) {
  const toto::ResumeDecisionLog decisions;
  toto::OfferContext context = offerOf(at(60.0), at(30.0));
  context.remote = toto::ResumePosition{};
  EXPECT_EQ(toto::shouldOffer(decisions, context), toto::OfferRefusal::NoLocator);
}

TEST(ShouldOffer, ARejectedPositionIsNotAskedAgain) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.0), false, "s-1", 100, true));
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(60.0), at(30.0))), toto::OfferRefusal::AlreadyResolved);
  // Reconnecting ten times cannot become ten identical questions: a position
  // that did not move counts as the one already answered.
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(60.3), at(30.0))), toto::OfferRefusal::AlreadyResolved);
  // A really new position is asked, even though the previous one was refused.
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(72.0), at(30.0))), toto::OfferRefusal::None);
}

TEST(ShouldOffer, AnAcceptedPositionIsNotAskedAgainEither) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.0), true, "s-1", 100, true));
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(60.0), at(60.0))), toto::OfferRefusal::SamePosition);
  EXPECT_EQ(toto::shouldOffer(decisions, offerOf(at(60.0), at(30.0))), toto::OfferRefusal::AlreadyResolved);
}

TEST(OfferRefusalName, EveryReasonHasAStableName) {
  EXPECT_STREQ(toto::offerRefusalName(toto::OfferRefusal::None), "offer");
  EXPECT_STREQ(toto::offerRefusalName(toto::OfferRefusal::OtherAccount), "other_account");
  EXPECT_STREQ(toto::offerRefusalName(toto::OfferRefusal::AlreadyResolved), "already_resolved");
  EXPECT_STREQ(toto::offerRefusalName(toto::OfferRefusal::NoLocator), "no_locator");
}

// ---------------------------------------------------------------------------
// The memory of what was answered
// ---------------------------------------------------------------------------

TEST(DecisionLog, RefusesToRememberWhatItCannotIdentify) {
  toto::ResumeDecisionLog decisions;
  EXPECT_FALSE(decisions.remember("", at(10.0), true, "s", 1, true));
  EXPECT_FALSE(decisions.remember("b1", toto::ResumePosition{}, true, "s", 1, true));
  EXPECT_EQ(decisions.size(), 0u);
}

TEST(DecisionLog, OneAnswerPerBookAndTheLastOneWins) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.0), false, "s-1", 100, true));
  ASSERT_TRUE(decisions.remember("b1", at(80.0), true, "s-2", 200, true));
  EXPECT_EQ(decisions.size(), 1u);
  const toto::ResumeDecision* decision = decisions.find("b1");
  ASSERT_NE(decision, nullptr);
  EXPECT_TRUE(decision->accepted);
  EXPECT_EQ(decision->suggestionId, "s-2");
}

TEST(DecisionLog, KeepsOnlyTheMostRecentBooks) {
  toto::ResumeDecisionLog decisions;
  for (size_t index = 0; index < toto::RESUME_MAX_BOOKS + 5; ++index) {
    ASSERT_TRUE(decisions.remember("book-" + std::to_string(index), at(10.0 + static_cast<double>(index)), false, {},
                                   1000 + index, true));
  }
  EXPECT_EQ(decisions.size(), toto::RESUME_MAX_BOOKS);
  EXPECT_EQ(decisions.find("book-0"), nullptr);
  EXPECT_NE(decisions.find("book-" + std::to_string(toto::RESUME_MAX_BOOKS + 4)), nullptr);
}

TEST(DecisionLog, ForgetAndClearDropAnswers) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.0), false, {}, 1, true));
  EXPECT_TRUE(decisions.forget("b1"));
  EXPECT_FALSE(decisions.forget("b1"));
  ASSERT_TRUE(decisions.remember("b2", at(60.0), false, {}, 1, true));
  decisions.clear();
  EXPECT_EQ(decisions.size(), 0u);
}

TEST(DecisionLog, AnAnswerGivenOfflineStaysPendingUntilTheHubTakesIt) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.0), true, "s-1", 100, false));
  ASSERT_TRUE(decisions.remember("b2", at(20.0), false, "s-2", 50, false));
  // An answer with no suggestion id has nothing to deliver, so it is not
  // pending: it would retry a request the hub never expects.
  ASSERT_TRUE(decisions.remember("b3", at(20.0), false, {}, 10, false));

  const auto pending = decisions.pendingResolutions();
  ASSERT_EQ(pending.size(), 2u);
  EXPECT_EQ(pending[0].bookHash, "b2");
  EXPECT_EQ(pending[1].bookHash, "b1");

  EXPECT_FALSE(decisions.markSynced("b1", "s-9"));
  EXPECT_TRUE(decisions.markSynced("b1", "s-1"));
  EXPECT_EQ(decisions.pendingResolutions().size(), 1u);
}

TEST(DecisionLog, SurvivesAReboot) {
  toto::ResumeDecisionLog decisions;
  ASSERT_TRUE(decisions.remember("b1", at(60.5, 120, 400, "/body/p[7]"), false, "s-1", 1700000000, false));
  ASSERT_TRUE(decisions.remember("b2", at(12.0), true, {}, 1700000100, true));

  const toto::ResumeDecisionLog restored = toto::ResumeDecisionLog::parse(decisions.serialize());
  ASSERT_EQ(restored.size(), 2u);
  const toto::ResumeDecision* first = restored.find("b1");
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(first->accepted);
  EXPECT_FALSE(first->synced);
  EXPECT_EQ(first->suggestionId, "s-1");
  EXPECT_TRUE(first->hasPercentage);
  EXPECT_NEAR(first->percentage, 60.5, 0.01);
  EXPECT_EQ(first->at, 1700000000u);

  const toto::ResumeDecision* second = restored.find("b2");
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(second->accepted);
  EXPECT_TRUE(second->synced);

  // And what was refused before the reboot is still refused after it.
  EXPECT_EQ(toto::shouldOffer(restored, offerOf(at(60.5, 120, 400, "/body/p[7]"), at(30.0))),
            toto::OfferRefusal::AlreadyResolved);
}

TEST(DecisionLog, GarbageIsDroppedInsteadOfCrashingTheScreen) {
  EXPECT_EQ(toto::ResumeDecisionLog::parse("").size(), 0u);
  EXPECT_EQ(toto::ResumeDecisionLog::parse("v9\nb1\tevent:x\ta\t10\t1\t\t1\n").size(), 0u);
  EXPECT_EQ(toto::ResumeDecisionLog::parse("v1\nnot-enough\tfields\n").size(), 0u);
  EXPECT_EQ(toto::ResumeDecisionLog::parse("v1\n\t\ta\t\t1\t\t1\n").size(), 0u);
  EXPECT_EQ(toto::ResumeDecisionLog::parse("v1\nb1\tevent:x\ta\tnot-a-number\tnope\t\t1\n").size(), 1u);
}

// ---------------------------------------------------------------------------
// What ends up on screen
// ---------------------------------------------------------------------------

TEST(SourceName, ATechnicalIdIsNotAName) {
  EXPECT_EQ(toto::resumeSourceName("Kobo de Ana", "kobo"), "Kobo de Ana");
  EXPECT_EQ(toto::resumeSourceName("  ", "kobo"), "kobo");
  EXPECT_EQ(toto::resumeSourceName("0f8b1a4c-1111-2222-3333-444455556666", "kobo"), "kobo");
  EXPECT_TRUE(toto::resumeSourceName("deadbeef", "").empty());
  EXPECT_TRUE(toto::resumeSourceName("", "").empty());
}

TEST(TrustedTime, OnlyAnExactOrAnchoredClockEarnsADate) {
  EXPECT_TRUE(toto::resumeTrustedTimePrecision("exact"));
  EXPECT_TRUE(toto::resumeTrustedTimePrecision("ANCHORED"));
  EXPECT_FALSE(toto::resumeTrustedTimePrecision("approximate"));
  EXPECT_FALSE(toto::resumeTrustedTimePrecision("unknown"));
  EXPECT_FALSE(toto::resumeTrustedTimePrecision(""));
}

TEST(DescribeResume, ShowsBothSidesAndWhereItCameFrom) {
  toto::DescribeContext context;
  context.remote = at(60.0, 240, 400, "/body/p[7]");
  context.local = at(30.0, 120, 400);
  context.hasLocal = true;
  context.originName = "Kobo de Ana";
  context.originPlatform = "kobo";

  const toto::ResumeCard card = toto::describeResume(context);
  EXPECT_EQ(card.originName, "Kobo de Ana");
  EXPECT_TRUE(card.here.known);
  EXPECT_TRUE(card.here.hasPage);
  EXPECT_EQ(card.here.page, 120u);
  EXPECT_EQ(card.here.totalPages, 400u);
  EXPECT_TRUE(card.there.known);
  EXPECT_NEAR(card.there.percentage, 60.0, 1e-9);
  EXPECT_FALSE(card.backwards);
  EXPECT_FALSE(card.smallJump);
  EXPECT_FALSE(card.approximate);
  EXPECT_FALSE(card.trustedTime);
}

TEST(DescribeResume, WarnsWhenTheJumpGoesBackwards) {
  toto::DescribeContext context;
  context.remote = at(20.0, 80, 400, "/body/p[2]");
  context.local = at(70.0, 280, 400);
  context.hasLocal = true;
  const toto::ResumeCard card = toto::describeResume(context);
  EXPECT_TRUE(card.backwards);
  EXPECT_FALSE(card.smallJump);
}

TEST(DescribeResume, SaysWhenTheDifferenceIsSmall) {
  toto::DescribeContext context;
  context.remote = at(31.5, 126, 400, "/body/p[4]");
  context.local = at(30.0, 120, 400);
  context.hasLocal = true;
  const toto::ResumeCard card = toto::describeResume(context);
  EXPECT_FALSE(card.backwards);
  EXPECT_TRUE(card.smallJump);
}

TEST(DescribeResume, SaysWhenTheDestinationIsApproximate) {
  toto::DescribeContext context;
  context.remote = at(60.0);
  context.local = at(30.0);
  context.hasLocal = true;
  const toto::ResumeCard card = toto::describeResume(context);
  EXPECT_TRUE(card.approximate);
  EXPECT_TRUE(card.originName.empty());
  EXPECT_FALSE(card.here.hasPage);
}

TEST(DescribeResume, ShowsTheDateOnlyWithATrustworthyClock) {
  toto::DescribeContext context;
  context.remote = at(60.0, 240, 400, "/body/p[7]");
  context.hasOccurredAt = true;
  context.timePrecision = "exact";
  EXPECT_TRUE(toto::describeResume(context).trustedTime);

  context.timePrecision = "approximate";
  EXPECT_FALSE(toto::describeResume(context).trustedTime);

  context.timePrecision = "exact";
  context.hasOccurredAt = false;
  EXPECT_FALSE(toto::describeResume(context).trustedTime);
}

TEST(DescribeResume, WithNoLocalPositionThereIsNothingToWarnAbout) {
  toto::DescribeContext context;
  context.remote = at(60.0, 240, 400, "/body/p[7]");
  context.hasLocal = false;
  const toto::ResumeCard card = toto::describeResume(context);
  EXPECT_FALSE(card.here.known);
  EXPECT_FALSE(card.backwards);
  EXPECT_FALSE(card.smallJump);
}

TEST(SummarizePosition, ClampsWhatWouldNotFitOnAScreen) {
  const toto::PositionSummary high = toto::summarizePosition(at(140.0));
  EXPECT_NEAR(high.percentage, 100.0, 1e-9);
  const toto::PositionSummary low = toto::summarizePosition(at(-5.0));
  EXPECT_NEAR(low.percentage, 0.0, 1e-9);
  EXPECT_FALSE(toto::summarizePosition(toto::ResumePosition{}).known);
}

}  // namespace
