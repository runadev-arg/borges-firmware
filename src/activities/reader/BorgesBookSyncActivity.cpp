#include "BorgesBookSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "BorgesCredentialStore.h"
#include "BorgesSyncClient.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BorgesBookSyncActivity::onEnter() {
  Activity::onEnter();
  if (!BORGES_CREDENTIALS.paired()) {
    message = tr(STR_BORGES_BOOK_PAIR_FIRST);
  } else if (WiFi.status() == WL_CONNECTED) {
    working = true;
  } else {
    auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
    if (!wifi) {
      message =
          std::string(tr(STR_BORGES_FAILED)) + ": " + borges::SyncClient::resultName(borges::SyncClient::Result::LOW_MEMORY);
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
      working = !result.isCancelled && WiFi.status() == WL_CONNECTED;
      if (!working) message = tr(STR_BORGES_WIFI_REQUIRED);
      requestUpdate();
    });
  }
  requestUpdate();
}

void BorgesBookSyncActivity::performStep() {
  if (mode != Mode::Sync) {
    const auto result = borges::SyncClient::fetchProgress(bookHash, mode == Mode::LatestOther);
    working = false;
    if (result.result != borges::SyncClient::Result::OK) {
      message = std::string(tr(STR_BORGES_FAILED)) + ": " + borges::SyncClient::resultName(result.result);
    } else if (!result.found) {
      message = (mode == Mode::LatestOther ? tr(STR_BORGES_BOOK_NO_OTHER) : tr(STR_BORGES_BOOK_NO_PROGRESS));
    } else {
      // Only the durable pending candidate exists here: reopening asks consent
      // and saves the accepted intent before applying the mapped location.
      activityManager.goToReader(epubPath);
      return;
    }
  } else {
    const auto result = borges::SyncClient::syncOnce();
    sent += result.acknowledged;
    received += result.pulled;
    ++rounds;
    if (result.result != borges::SyncClient::Result::OK) {
      working = false;
      message = std::string(tr(STR_BORGES_FAILED)) + ": " + borges::SyncClient::resultName(result.result);
    } else if (result.rejected > 0) {
      working = false;
      message = tr(STR_BORGES_BOOK_REJECTED);
    } else {
      working = result.hasMore || result.queueDepth > 0;
      if (working && rounds >= 64) {
        working = false;
        message = tr(STR_BORGES_BOOK_MORE_PENDING);
      } else {
        char summary[128];
        std::snprintf(summary, sizeof(summary), tr(STR_BORGES_SYNC_RESULT), static_cast<unsigned>(sent),
                      static_cast<unsigned>(received));
        message = summary;
      }
    }
  }
  requestUpdate();
}

void BorgesBookSyncActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (!working && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    activityManager.goToReader(epubPath);
    return;
  }
  if (working) {
    requestUpdateAndWait();
    performStep();
  }
}

void BorgesBookSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const char* heading = mode == Mode::Sync
                            ? tr(STR_BORGES_SYNC_NOW)
                            : (mode == Mode::Latest ? tr(STR_BORGES_BOOK_FETCH_LATEST) : tr(STR_BORGES_BOOK_FETCH_OTHER));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, heading);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const std::string text = working ? std::string(tr(STR_BORGES_WORKING)) + "\n" + message : message;
  for (const auto& line :
       renderer.wrappedText(UI_10_FONT_ID, text.c_str(), width - metrics.contentSidePadding * 2, 8)) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), working ? "" : tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
