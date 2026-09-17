#include "ConfirmationActivity.h"

#include <I18n.h>

#include <utility>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading) {
  if (!body.empty()) bodyLines.push_back(body);
}

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                                           std::vector<std::string> lines, std::string cancelLabel,
                                           std::string confirmLabel)
    : Activity("Confirmation", renderer, mappedInput),
      heading(std::move(heading)),
      bodyLines(std::move(lines)),
      cancelLabel(std::move(cancelLabel)),
      confirmLabel(std::move(confirmLabel)) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  safeBodyLines.clear();
  safeBodyLines.reserve(bodyLines.size());
  for (const std::string& line : bodyLines) {
    if (line.empty()) {
      safeBodyLines.emplace_back();
      continue;
    }
    safeBodyLines.push_back(renderer.truncatedText(fontId, line.c_str(), maxWidth, EpdFontFamily::REGULAR));
  }

  // Text sits in the upper part of the screen so the confirmation popup
  // (centered) doesn't cover it. More lines start higher, for the same reason.
  startY = renderer.getScreenHeight() / (safeBodyLines.size() > 1 ? 8 : 6);

  const std::string cancel = cancelLabel.empty() ? std::string(I18N.get(StrId::STR_CANCEL)) : cancelLabel;
  const std::string confirm = confirmLabel.empty() ? std::string(I18N.get(StrId::STR_CONFIRM)) : confirmLabel;
  const char* options[] = {cancel.c_str(), confirm.c_str()};
  confirmPopup.show(safeHeading.c_str(), options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  for (const std::string& line : safeBodyLines) {
    if (!line.empty()) {
      renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    }
    currentY += lineHeight;
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
