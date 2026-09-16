#include "TotoSyncStatusActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "TotoCredentialStore.h"
#include "TotoSyncText.h"
#include "components/UITheme.h"
#include "fontIds.h"

void TotoSyncStatusActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void TotoSyncStatusActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void TotoSyncStatusActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int textWidth = width - metrics.contentSidePadding * 2;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TOTO_MENU_STATUS));

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // Nothing is allowed past this: the button hints own the bottom band, and a
  // sentence that grows in translation must run out of page, not cover them.
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - lineHeight;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight;

  // Written most-useful-first, so if a long translation does run out of page
  // what is lost is the explanation, never the code the reader came to copy.
  const auto line = [&](const std::string& text, int maxLines = 2,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    for (const std::string& row : renderer.wrappedText(UI_10_FONT_ID, text.c_str(), textWidth, maxLines, style)) {
      if (y > bottom) return;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, row.c_str(), true, style);
      y += lineHeight;
    }
  };
  const auto gap = [&] { y += lineHeight / 2; };
  const auto counted = [&](const char* format, size_t value) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), format, static_cast<unsigned>(value));
    line(text.data(), 1);
  };

  line(toto_ui::accountSentence(), 2, EpdFontFamily::BOLD);
  line(toto_ui::statusSentence(snapshot));
  line(toto_ui::lastSyncSentence(snapshot), 1);
  counted(tr(STR_TOTO_HELP_TO_SEND), snapshot.outbox);
  counted(tr(STR_TOTO_HELP_TO_APPLY), snapshot.inbox);
  if (snapshot.remotePositionWaiting) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), tr(STR_TOTO_RESUME_AT), remotePositionPercentage);
    line(text.data(), 1);
  }

  gap();
  std::array<char, 128> report{};
  std::snprintf(report.data(), report.size(), tr(STR_TOTO_HELP_REPORT),
                toto::supportHost(TOTO_CREDENTIALS.getBaseUrl()).c_str());
  line(report.data());

  std::array<char, 128> code{};
  std::snprintf(code.data(), code.size(), tr(STR_TOTO_HELP_CODE),
                toto::reportCode(CROSSPOINT_VERSION, TOTO_CREDENTIALS.getDeviceId()).c_str());
  line(code.data(), 2, EpdFontFamily::BOLD);
  line(tr(STR_TOTO_HELP_NO_SECRETS));

  gap();
  line(tr(STR_TOTO_HELP_WHAT), 3);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
