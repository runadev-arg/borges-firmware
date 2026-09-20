#pragma once
#include <string>

#include "activities/Activity.h"

class BorgesBookSyncActivity final : public Activity {
 public:
  enum class Mode { Sync, Latest, LatestOther };
  BorgesBookSyncActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path, std::string hash, Mode mode)
      : Activity("BorgesBookSync", renderer, input), epubPath(std::move(path)), bookHash(std::move(hash)), mode(mode) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }  // block background upload during explicit pull
  bool preventAutoSleep() override { return working; }

 private:
  std::string epubPath;
  std::string bookHash;
  Mode mode;
  bool working = false;
  unsigned rounds = 0;
  size_t sent = 0;
  size_t received = 0;
  std::string message;
  void performStep();
};
