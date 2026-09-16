#pragma once

#include <string>

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

}  // namespace toto_ui
