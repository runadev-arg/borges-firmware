#pragma once

#include "TotoSyncMenu.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Diagnosis and recovery for the Toto account: pairing by code, re-registering
 * the services a sign-in sets up, and dropping a reading position that is
 * waiting for a decision.
 *
 * It chooses, it does not act. The chosen row comes back as a MenuResult and
 * the Toto screen runs it, so there is exactly one place in the firmware that
 * connects to Wi-Fi and spends the session.
 */
class TotoSyncAdvancedActivity final : public Activity {
 public:
  TotoSyncAdvancedActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const toto::SyncSnapshot& snapshot)
      : Activity("TotoSyncAdvanced", renderer, mappedInput), snapshot(snapshot) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator navigator;
  toto::SyncSnapshot snapshot;
  int selectedIndex = 0;

  void activate();
  std::string rowTitle(int index) const;
};
