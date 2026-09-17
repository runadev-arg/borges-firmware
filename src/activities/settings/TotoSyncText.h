#pragma once

#include <string>
#include <vector>

#include "TotoResumeFlow.h"
#include "TotoSyncMenu.h"

// Turns the decisions TotoSyncMenu makes into sentences a reader understands.
// It is the only place that knows both the core enums and tr(), so the two
// Toto screens cannot drift into saying different things about one state.
namespace toto_ui {

// Name of the signed-in account, or why there is none.
std::string accountSentence();

// One line on where sync stands right now.
std::string statusSentence(const toto::SyncSnapshot& snapshot);

// "Last sync: ..." with the age already bucketed by the core.
std::string lastSyncSentence(const toto::SyncSnapshot& snapshot);

// One readable line for a position: page out of total and percentage. Never
// the anchor and never the event id -- those tell a reader nothing.
std::string positionSentence(const toto::PositionSummary& summary);

// The body of the "continue from another device?" question: where the reader
// is, where the other device left them, and the warnings that change the
// answer. The heading and the two option labels come from the calls below, so
// every screen asks it with the same words.
std::vector<std::string> resumeCardLines(const toto::ResumeCard& card);
std::string resumeHeading();
std::string resumeGoLabel();
std::string resumeStayLabel();

}  // namespace toto_ui
