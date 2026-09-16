#pragma once

#include "TotoSyncMenu.h"
#include "activities/Activity.h"

/**
 * Read-only account status and the route to reporting a problem.
 *
 * It answers the two questions a reader has when sync looks wrong -- what is
 * still pending, and when did it last work -- and hands over the firmware
 * version and the device id to quote in a report. Nothing secret reaches this
 * screen: the report code is built without the session token.
 */
class TotoSyncStatusActivity final : public Activity {
 public:
  TotoSyncStatusActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const toto::SyncSnapshot& snapshot,
                         double remotePositionPercentage)
      : Activity("TotoSyncStatus", renderer, mappedInput),
        snapshot(snapshot),
        remotePositionPercentage(remotePositionPercentage) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  toto::SyncSnapshot snapshot;
  double remotePositionPercentage = 0;
};
