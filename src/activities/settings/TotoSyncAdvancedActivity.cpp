#include "TotoSyncAdvancedActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

void TotoSyncAdvancedActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void TotoSyncAdvancedActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touched = selectedIndex;
  const auto touch = handleListTouch(touched, toto::ADVANCED_ROW_COUNT, contentTop, contentHeight, false);
  if (touch != ListTouchResult::None) {
    selectedIndex = touched;
    if (touch == ListTouchResult::Activated) activate();
    return;
  }

  navigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % toto::ADVANCED_ROW_COUNT;
    requestUpdate();
  });
  navigator.onPrevious([this] {
    selectedIndex = (selectedIndex + toto::ADVANCED_ROW_COUNT - 1) % toto::ADVANCED_ROW_COUNT;
    requestUpdate();
  });
}

void TotoSyncAdvancedActivity::activate() {
  const auto row = static_cast<toto::AdvancedRow>(selectedIndex);
  // A row that cannot run right now stays on screen dimmed rather than
  // disappearing: a menu that changes shape is harder to learn than one with a
  // greyed-out line.
  if (!toto::advancedRowEnabled(row, snapshot)) return;
  setResult(MenuResult{.action = selectedIndex});
  finish();
}

std::string TotoSyncAdvancedActivity::rowTitle(int index) const {
  switch (static_cast<toto::AdvancedRow>(index)) {
    case toto::AdvancedRow::PairWithCode:
      return snapshot.pairingPending ? tr(STR_TOTO_CHECK_PAIRING) : tr(STR_TOTO_PAIR_DEVICE);
    case toto::AdvancedRow::RepairServices:
      return tr(STR_TOTO_REPAIR_SERVICES);
    case toto::AdvancedRow::DiscardRemotePosition:
      break;
  }
  return tr(STR_TOTO_DISMISS_RESUME);
}

void TotoSyncAdvancedActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TOTO_MENU_ADVANCED));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, toto::ADVANCED_ROW_COUNT, selectedIndex,
      [this](int index) { return rowTitle(index); }, nullptr, nullptr, nullptr, false,
      [this](int index) { return !toto::advancedRowEnabled(static_cast<toto::AdvancedRow>(index), snapshot); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
