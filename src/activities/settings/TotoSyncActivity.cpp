#include "TotoSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <array>
#include <cstdio>

#include "MappedInputManager.h"
#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoPairingClient.h"
#include "TotoSyncClient.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
}

void TotoSyncActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  working = false;
  resultText.clear();
  refreshDecision();
  requestUpdate();
}

void TotoSyncActivity::loop() {
  if (working) {
    requestUpdateAndWait();
    performPendingAction();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + 86;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touched = selectedIndex;
  const auto touch = handleListTouch(touched, MENU_ITEMS, contentTop, contentHeight, false);
  if (touch != ListTouchResult::None) {
    selectedIndex = touched;
    if (touch == ListTouchResult::Activated) activate();
    return;
  }

  navigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });
  navigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void TotoSyncActivity::activate() {
  if (selectedIndex == 0) {
    ensureWifiThen(Action::PairOrClaim);
  } else if (selectedIndex == 1 && TOTO_CREDENTIALS.paired()) {
    ensureWifiThen(Action::Sync);
  } else if (selectedIndex == 2 && progressDecision) {
    pendingAction = Action::AcceptProgress;
    working = true;
    requestUpdate();
  } else if (selectedIndex == 3 && progressDecision) {
    pendingAction = Action::DismissProgress;
    working = true;
    requestUpdate();
  } else {
    resultText = tr(STR_TOTO_NOT_PAIRED);
    requestUpdate();
  }
}

void TotoSyncActivity::ensureWifiThen(Action action) {
  pendingAction = action;
  resultText.clear();
  if (WiFi.status() == WL_CONNECTED) {
    working = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             resultText = tr(STR_TOTO_WIFI_REQUIRED);
                             requestUpdate();
                             return;
                           }
                           working = true;
                           requestUpdate();
                         });
}

void TotoSyncActivity::performPendingAction() {
  if (pendingAction == Action::PairOrClaim) {
    if (TOTO_CREDENTIALS.paired()) {
      resultText = toto::bootstrapCrossPointServices() ? tr(STR_TOTO_PAIR_SUCCESS) : tr(STR_TOTO_FAILED);
    } else {
      const auto result =
          TOTO_CREDENTIALS.pairingPending() ? toto::PairingClient::pollAndClaim() : toto::PairingClient::request();
      if (result == toto::PairingClient::Result::OK || result == toto::PairingClient::Result::PENDING) {
        resultText = tr(STR_TOTO_PAIR_PENDING);
      } else if (result == toto::PairingClient::Result::PAIRED) {
        resultText = tr(STR_TOTO_PAIR_SUCCESS);
      } else {
        resultText = std::string(tr(STR_TOTO_FAILED)) + ": " + toto::PairingClient::resultName(result);
        // Diagnostic tail: transport stage ("tls:-188", "tcp", ...) or the HTTP
        // status when the server did answer. Without this the device can only
        // say "network_error" and the field-debugging loop is blind.
        if (!toto::PairingClient::lastErrorDetail.empty()) {
          resultText += " [" + toto::PairingClient::lastErrorDetail + "]";
        } else if (toto::PairingClient::lastHttpCode > 0) {
          resultText += " [http " + std::to_string(toto::PairingClient::lastHttpCode) + "]";
        }
      }
    }
  } else if (pendingAction == Action::Sync) {
    const toto::SyncClient::Outcome outcome = toto::SyncClient::syncOnce();
    if (outcome.result == toto::SyncClient::Result::OK) {
      std::array<char, 96> text{};
      std::snprintf(text.data(), text.size(), tr(STR_TOTO_SYNC_RESULT), static_cast<unsigned>(outcome.acknowledged),
                    static_cast<unsigned>(outcome.pulled));
      resultText = text.data();
    } else {
      resultText = std::string(tr(STR_TOTO_FAILED)) + ": " + toto::SyncClient::resultName(outcome.result);
    }
  } else if (progressDecision) {
    const bool accept = pendingAction == Action::AcceptProgress;
    // A saved location is usable offline and survives a server-side dismissal.
    // Server suggestion acknowledgement is optional; the local decision is durable.
    const bool stored =
        accept ? TOTO_QUEUE.acceptProgress(*progressDecision) : TOTO_QUEUE.dismissProgress(*progressDecision);
    if (!stored) {
      resultText = tr(STR_TOTO_FAILED);
    } else if (accept) {
      resultText = tr(STR_TOTO_RESUME_ACCEPTED);
    } else {
      resultText = tr(STR_TOTO_RESUME_DISMISSED);
    }
  }
  refreshDecision();
  working = false;
  requestUpdate();
}

void TotoSyncActivity::refreshDecision() { progressDecision = TOTO_QUEUE.nextProgressDecision({}, true); }

void TotoSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TOTO_SYNC));

  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const char* pairingState = TOTO_CREDENTIALS.paired() ? tr(STR_TOTO_PAIRED) : tr(STR_TOTO_NOT_PAIRED);
  renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 14, pairingState, true, EpdFontFamily::BOLD);

  if (TOTO_CREDENTIALS.pairingPending()) {
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 40, tr(STR_TOTO_PAIR_WEB));
  } else {
    std::array<char, 96> queueLine{};
    std::snprintf(queueLine.data(), queueLine.size(), tr(STR_TOTO_QUEUE_STATUS),
                  static_cast<unsigned>(TOTO_QUEUE.depth()), static_cast<unsigned>(TOTO_QUEUE.inboxDepth()));
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 40, queueLine.data());
  }

  if (TOTO_CREDENTIALS.pairingPending()) {
    std::array<char, 96> codeLine{};
    std::snprintf(codeLine.data(), codeLine.size(), tr(STR_TOTO_PAIR_CODE),
                  TOTO_CREDENTIALS.getPairingUserCode().c_str());
    renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 64, codeLine.data(), true, EpdFontFamily::BOLD);
  } else if (progressDecision) {
    std::array<char, 96> resumeLine{};
    std::snprintf(resumeLine.data(), resumeLine.size(), tr(STR_TOTO_RESUME_AT), progressDecision->percentage);
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

  const int contentTop = metrics.topPadding + metrics.headerHeight + 86;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, MENU_ITEMS, selectedIndex,
      [](int index) {
        if (index == 0) {
          if (TOTO_CREDENTIALS.paired()) return std::string(tr(STR_TOTO_REPAIR_SERVICES));
          return std::string(TOTO_CREDENTIALS.pairingPending() ? tr(STR_TOTO_CHECK_PAIRING) : tr(STR_TOTO_PAIR_DEVICE));
        }
        if (index == 1) return std::string(tr(STR_TOTO_SYNC_NOW));
        if (index == 2) return std::string(tr(STR_TOTO_ACCEPT_RESUME));
        return std::string(tr(STR_TOTO_DISMISS_RESUME));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 1 && !TOTO_CREDENTIALS.paired()) return std::string("[") + tr(STR_TOTO_NOT_PAIRED) + "]";
        if (index >= 2 && !progressDecision) return std::string("[") + tr(STR_TOTO_NONE) + "]";
        return std::string();
      },
      true);

  if (working) {
    GUI.drawPopup(renderer, tr(STR_TOTO_WORKING));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
