#pragma once

#include <optional>
#include <string>

#include "TotoDeviceLogin.h"
#include "TotoDurableQueue.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TotoSyncActivity final : public Activity {
 public:
  explicit TotoSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TotoSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return working; }
  bool preventAutoSleep() override { return working; }

 private:
  enum class Action { SignIn, PairOrClaim, Sync, AcceptProgress, DismissProgress };

  ButtonNavigator navigator;
  int selectedIndex = 0;
  bool working = false;
  Action pendingAction = Action::SignIn;
  std::string resultText;
  std::optional<toto::ProgressInboxItem> progressDecision;

  // Held only between the keyboard screens and the request that spends them.
  // Both are wiped as soon as the login returns, and neither is ever written.
  std::string pendingIdentifier;
  std::string pendingPassword;
  // A session that authenticated but is still waiting for the reader to say
  // what happens to the events queued for the previous account.
  toto::DeviceSession pendingSession;

  void activate();
  void ensureWifiThen(Action action);
  void performPendingAction();
  void refreshDecision();

  void askUsername();
  void askPassword();
  void performSignIn();
  void applySession(bool discardPreviousAccount);
  void confirmAccountSwitch(size_t pendingEvents);
  void forgetPendingCredentials();

  std::string accountLine() const;
  std::string statusLine() const;
  static const char* nextStepText(toto::NextStep step);
};
