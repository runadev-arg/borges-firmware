#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::vector<std::string> bodyLines;
  std::string cancelLabel;
  std::string confirmLabel;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::vector<std::string> safeBodyLines;
  OptionPopup confirmPopup;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  // A question that needs more than one line, and whose options say what they
  // do instead of "Confirm" and "Cancel". Comparing two reading positions is
  // the case this exists for: "Go there" and "Stay here" are answers, while
  // "Cancel" leaves the reader guessing which position they just kept.
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                       std::vector<std::string> lines, std::string cancelLabel, std::string confirmLabel);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
