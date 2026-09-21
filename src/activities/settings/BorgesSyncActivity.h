#pragma once

#include <array>
#include <optional>
#include <string>

#include "BorgesDurableQueue.h"
#include "activities/UiListActivity.h"
#include "util/ButtonNavigator.h"

class BorgesSyncActivity final : public UiListActivity {
 public:
  explicit BorgesSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("BorgesSync", renderer, mappedInput) {}

  void onEnter() override;
  bool skipLoopDelay() override { return working; }

 private:
  enum class Action { PairOrClaim, Sync, AcceptProgress, DismissProgress };

  static constexpr int MENU_ITEMS = 4;
  std::array<freeink::ui::ListItem, MENU_ITEMS> rowItems{};
  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void drawChrome() override;
  void drawFooter() override;
  bool working = false;
  Action pendingAction = Action::PairOrClaim;
  std::string resultText;
  std::optional<borges::ProgressInboxItem> progressDecision;

  void activate();
  void ensureWifiThen(Action action);
  void performPendingAction();
  void refreshDecision();
};
