#pragma once

#include <optional>
#include <string>

#include "TotoDurableQueue.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TotoSyncActivity final : public Activity {
 public:
  explicit TotoSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TotoSync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return working; }

 private:
  enum class Action { PairOrClaim, Sync, AcceptProgress, DismissProgress };

  ButtonNavigator navigator;
  int selectedIndex = 0;
  bool working = false;
  Action pendingAction = Action::PairOrClaim;
  std::string resultText;
  std::optional<toto::ProgressInboxItem> progressDecision;

  void activate();
  void ensureWifiThen(Action action);
  void performPendingAction();
  void refreshDecision();
};
