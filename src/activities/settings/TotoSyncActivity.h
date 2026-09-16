#pragma once

#include <optional>
#include <string>

#include "TotoDeviceLogin.h"
#include "TotoDurableQueue.h"
#include "TotoSyncMenu.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * The Toto account screen: one row per thing a reader wants to do -- account,
 * sync, library, status and help -- with diagnosis and recovery folded into
 * Advanced. Every capability appears exactly once.
 *
 * This is also the only screen that connects and spends the session. The
 * Advanced screen chooses; this one acts, so a request cannot be started from
 * two places at once.
 */
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
  enum class Action { SignIn, Sync, RepairServices, PairOrClaim, AcceptProgress, DismissProgress };

  ButtonNavigator navigator;
  int selectedIndex = 0;
  bool working = false;
  Action pendingAction = Action::SignIn;
  std::string resultText;
  std::optional<toto::ProgressInboxItem> progressDecision;
  toto::SyncSnapshot snapshot;
  // The position already put to the reader this visit. Backing out of the
  // question is not an answer, so the same one is never asked twice; a
  // position that arrives later carries its own id and is asked on its own.
  std::optional<std::string> offeredSuggestionId;

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
  void refreshSnapshot();

  void openAccount();
  void openLibrary();
  void openStatusAndHelp();
  void openAdvanced();
  void offerRemotePosition();

  void askUsername();
  void askPassword();
  void performSignIn();
  void applySession(bool discardPreviousAccount);
  void confirmAccountSwitch(size_t pendingEvents);
  void forgetPendingCredentials();

  std::string rowTitle(int index) const;
  std::string statusLine() const;
  static const char* nextStepText(toto::NextStep step);
};
