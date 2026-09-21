#include "BorgesSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <array>
#include <cstdio>

#include "BorgesCredentialStore.h"
#include "BorgesDurableQueue.h"
#include "BorgesPairingClient.h"
#include "BorgesSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

void BorgesSyncActivity::onEnter() {
  UiListActivity::onEnter();
  nav.selected = 0;
  working = false;
  resultText.clear();
  refreshDecision();
  requestUpdate();
}

bool BorgesSyncActivity::handleCustomInput() {
  if (!working) return false;
  requestUpdateAndWait();
  performPendingAction();
  return true;
}

void BorgesSyncActivity::activateIndex(int index) {
  if (working || index < 0 || index >= MENU_ITEMS) return;
  nav.selected = index;
  app.clearTapFlash();
  activate();
}

void BorgesSyncActivity::activate() {
  if (nav.selected == 0) {
    ensureWifiThen(Action::PairOrClaim);
  } else if (nav.selected == 1 && BORGES_CREDENTIALS.paired()) {
    ensureWifiThen(Action::Sync);
  } else if (nav.selected == 2 && progressDecision) {
    pendingAction = Action::AcceptProgress;
    working = true;
    requestUpdate();
  } else if (nav.selected == 3 && progressDecision) {
    pendingAction = Action::DismissProgress;
    working = true;
    requestUpdate();
  } else {
    resultText = tr(STR_BORGES_NOT_PAIRED);
    requestUpdate();
  }
}

void BorgesSyncActivity::ensureWifiThen(Action action) {
  pendingAction = action;
  resultText.clear();
  if (WiFi.status() == WL_CONNECTED) {
    working = true;
    requestUpdate();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    resultText = tr(STR_BORGES_FAILED);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      resultText = tr(STR_BORGES_WIFI_REQUIRED);
      requestUpdate();
      return;
    }
    working = true;
    requestUpdate();
  });
}

void BorgesSyncActivity::performPendingAction() {
  if (pendingAction == Action::PairOrClaim) {
    if (BORGES_CREDENTIALS.paired()) {
      resultText = borges::bootstrapBorgesServices() ? tr(STR_BORGES_PAIR_SUCCESS) : tr(STR_BORGES_FAILED);
    } else {
      const auto result = BORGES_CREDENTIALS.pairingPending() ? borges::PairingClient::pollAndClaim()
                                                              : borges::PairingClient::request();
      if (result == borges::PairingClient::Result::OK || result == borges::PairingClient::Result::PENDING) {
        resultText = tr(STR_BORGES_PAIR_PENDING);
      } else if (result == borges::PairingClient::Result::PAIRED) {
        resultText = tr(STR_BORGES_PAIR_SUCCESS);
      } else {
        resultText = std::string(tr(STR_BORGES_FAILED)) + ": " + borges::PairingClient::resultName(result);
        // Diagnostic tail: transport stage ("tls:-188", "tcp", ...) or the HTTP
        // status when the server did answer. Without this the device can only
        // say "network_error" and the field-debugging loop is blind.
        if (!borges::PairingClient::lastErrorDetail.empty()) {
          resultText += " [" + borges::PairingClient::lastErrorDetail + "]";
        } else if (borges::PairingClient::lastHttpCode > 0) {
          resultText += " [http " + std::to_string(borges::PairingClient::lastHttpCode) + "]";
        }
      }
    }
  } else if (pendingAction == Action::Sync) {
    const borges::SyncClient::Outcome outcome = borges::SyncClient::syncOnce();
    if (outcome.result == borges::SyncClient::Result::OK) {
      std::array<char, 96> text{};
      std::snprintf(text.data(), text.size(), tr(STR_BORGES_SYNC_RESULT), static_cast<unsigned>(outcome.acknowledged),
                    static_cast<unsigned>(outcome.pulled));
      resultText = text.data();
    } else {
      resultText = std::string(tr(STR_BORGES_FAILED)) + ": " + borges::SyncClient::resultName(outcome.result);
    }
  } else if (progressDecision) {
    const bool accept = pendingAction == Action::AcceptProgress;
    // A saved location is usable offline and survives a server-side dismissal.
    // Server suggestion acknowledgement is optional; the local decision is durable.
    const bool stored =
        accept ? BORGES_QUEUE.acceptProgress(*progressDecision) : BORGES_QUEUE.dismissProgress(*progressDecision);
    if (!stored) {
      resultText = tr(STR_BORGES_FAILED);
    } else if (accept) {
      resultText = tr(STR_BORGES_RESUME_ACCEPTED);
    } else {
      resultText = tr(STR_BORGES_RESUME_DISMISSED);
    }
  }
  refreshDecision();
  working = false;
  requestUpdate();
}

void BorgesSyncActivity::refreshDecision() { progressDecision = BORGES_QUEUE.nextProgressDecision({}, true); }

void BorgesSyncActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_BORGES_SYNC));

  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const char* pairingState = BORGES_CREDENTIALS.paired() ? tr(STR_BORGES_PAIRED) : tr(STR_BORGES_NOT_PAIRED);
  renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 14, pairingState, true, EpdFontFamily::BOLD);

  if (BORGES_CREDENTIALS.pairingPending()) {
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 40, tr(STR_BORGES_PAIR_WEB));
  } else {
    std::array<char, 96> queueLine{};
    std::snprintf(queueLine.data(), queueLine.size(), tr(STR_BORGES_QUEUE_STATUS),
                  static_cast<unsigned>(BORGES_QUEUE.depth()), static_cast<unsigned>(BORGES_QUEUE.inboxDepth()));
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 40, queueLine.data());
  }

  if (BORGES_CREDENTIALS.pairingPending()) {
    std::array<char, 96> codeLine{};
    std::snprintf(codeLine.data(), codeLine.size(), tr(STR_BORGES_PAIR_CODE),
                  BORGES_CREDENTIALS.getPairingUserCode().c_str());
    renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 64, codeLine.data(), true, EpdFontFamily::BOLD);
  } else if (progressDecision) {
    std::array<char, 96> resumeLine{};
    std::snprintf(resumeLine.data(), resumeLine.size(), tr(STR_BORGES_RESUME_AT), progressDecision->percentage);
    const std::string source = progressDecision->sourceDeviceName.empty()
                                   ? std::string(resumeLine.data())
                                   : progressDecision->sourceDeviceName + " — " + resumeLine.data();
    const auto clipped = renderer.truncatedText(UI_10_FONT_ID, source.c_str(), width - 2 * metrics.contentSidePadding);
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 64, clipped.c_str(), true, EpdFontFamily::BOLD);
  } else if (!resultText.empty()) {
    // Diagnostic tails ("[clock ip:... dns:fail ...]") exceed one screen line;
    // wrap instead of clipping the very detail the failure screen exists for.
    const int contentWidth = width - 2 * metrics.contentSidePadding;
    auto lines = renderer.wrappedText(UI_10_FONT_ID, resultText.c_str(), contentWidth, 3);
    int lineY = summaryTop + 64;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, lineY, line.c_str());
      lineY += renderer.getLineHeight(UI_10_FONT_ID);
    }
  }
}

void BorgesSyncActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + 140), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});
  rowItems[0].label = BORGES_CREDENTIALS.paired() ? tr(STR_BORGES_REPAIR_SERVICES)
                                                  : (BORGES_CREDENTIALS.pairingPending() ? tr(STR_BORGES_CHECK_PAIRING)
                                                                                         : tr(STR_BORGES_PAIR_DEVICE));
  rowItems[1].label = tr(STR_BORGES_SYNC_NOW);
  rowItems[2].label = tr(STR_BORGES_ACCEPT_RESUME);
  rowItems[3].label = tr(STR_BORGES_DISMISS_RESUME);
  for (int index = 0; index < MENU_ITEMS; ++index) {
    rowItems[index].actionValue = static_cast<int16_t>(index);
    rowItems[index].value = index == 1 && !BORGES_CREDENTIALS.paired()
                                ? tr(STR_BORGES_NOT_PAIRED)
                                : (index >= 2 && !progressDecision ? tr(STR_BORGES_NONE) : "");
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = MENU_ITEMS;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void BorgesSyncActivity::drawFooter() {
  if (working) GUI.drawPopup(renderer, tr(STR_BORGES_WORKING));
  UiListActivity::drawFooter();
}
