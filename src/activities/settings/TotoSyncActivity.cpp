#include "TotoSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <variant>

#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoLoginClient.h"
#include "TotoPairingClient.h"
#include "TotoSyncAdvancedActivity.h"
#include "TotoSyncClient.h"
#include "TotoSyncStatusActivity.h"
#include "TotoSyncText.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MAX_CREDENTIAL_LENGTH = 128;

uint64_t nowUnixSeconds() {
  const std::time_t now = std::time(nullptr);
  return now > 0 ? static_cast<uint64_t>(now) : 0;
}

void wipe(std::string& secret) {
  std::fill(secret.begin(), secret.end(), '\0');
  secret.clear();
  secret.shrink_to_fit();
}
}  // namespace

void TotoSyncActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  working = false;
  offeredSuggestionId.reset();
  resultText.clear();
  refreshSnapshot();
  requestUpdate();
}

void TotoSyncActivity::onExit() {
  forgetPendingCredentials();
  Activity::onExit();
}

void TotoSyncActivity::loop() {
  if (working) {
    requestUpdateAndWait();
    performPendingAction();
    return;
  }
  // A position left by another device is a question, not a menu row: asking it
  // here is what keeps "accept" and "discard" from looking like two features.
  if (progressDecision && offeredSuggestionId != progressDecision->suggestionId) {
    offeredSuggestionId = progressDecision->suggestionId;
    offerRemotePosition();
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
  const auto touch = handleListTouch(touched, toto::MENU_ROW_COUNT, contentTop, contentHeight, false);
  if (touch != ListTouchResult::None) {
    selectedIndex = touched;
    if (touch == ListTouchResult::Activated) activate();
    return;
  }

  navigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % toto::MENU_ROW_COUNT;
    requestUpdate();
  });
  navigator.onPrevious([this] {
    selectedIndex = (selectedIndex + toto::MENU_ROW_COUNT - 1) % toto::MENU_ROW_COUNT;
    requestUpdate();
  });
}

void TotoSyncActivity::refreshSnapshot() {
  progressDecision = TOTO_QUEUE.nextProgressDecision();
  const uint64_t now = nowUnixSeconds();
  snapshot = toto::SyncSnapshot{
      .signedIn = TOTO_CREDENTIALS.paired(),
      .pairingPending = TOTO_CREDENTIALS.pairingPending(),
      .remotePositionWaiting = progressDecision.has_value(),
      .outbox = TOTO_QUEUE.depth(),
      .inbox = TOTO_QUEUE.inboxDepth(),
      .lastSuccessAt = TOTO_CREDENTIALS.getLastSyncAt(),
      .now = now,
      .session = TOTO_CREDENTIALS.state(now),
  };
}

void TotoSyncActivity::activate() {
  const auto row = static_cast<toto::MenuRow>(selectedIndex);
  if (!toto::rowEnabled(row, snapshot)) {
    resultText = tr(STR_TOTO_SIGNED_OUT);
    requestUpdate();
    return;
  }
  switch (row) {
    case toto::MenuRow::Account:
      openAccount();
      return;
    case toto::MenuRow::SyncNow:
      ensureWifiThen(Action::Sync);
      return;
    case toto::MenuRow::Library:
      openLibrary();
      return;
    case toto::MenuRow::StatusHelp:
      openStatusAndHelp();
      return;
    case toto::MenuRow::Advanced:
      openAdvanced();
      return;
  }
}

void TotoSyncActivity::openAccount() {
  if (!snapshot.signedIn) {
    askUsername();
    return;
  }
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TOTO_SIGN_OUT), toto_ui::accountSentence()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        toto::LoginClient::signOut();
        resultText = tr(STR_TOTO_SIGNED_OUT_DONE);
        refreshSnapshot();
        requestUpdate();
      });
}

void TotoSyncActivity::openLibrary() {
  // Picker mode: from here the reader wants to browse the books on the
  // account, not to edit the catalogue entry the sign-in created.
  startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true),
                         [this](const ActivityResult&) { refreshSnapshot(); });
}

void TotoSyncActivity::openStatusAndHelp() {
  const double percentage = progressDecision ? progressDecision->percentage : 0.0;
  startActivityForResult(std::make_unique<TotoSyncStatusActivity>(renderer, mappedInput, snapshot, percentage),
                         [this](const ActivityResult&) { refreshSnapshot(); });
}

void TotoSyncActivity::openAdvanced() {
  startActivityForResult(std::make_unique<TotoSyncAdvancedActivity>(renderer, mappedInput, snapshot),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* chosen = std::get_if<MenuResult>(&result.data);
                           if (chosen == nullptr) return;
                           switch (static_cast<toto::AdvancedRow>(chosen->action)) {
                             case toto::AdvancedRow::PairWithCode:
                               ensureWifiThen(Action::PairOrClaim);
                               return;
                             case toto::AdvancedRow::RepairServices:
                               ensureWifiThen(Action::RepairServices);
                               return;
                             case toto::AdvancedRow::DiscardRemotePosition:
                               ensureWifiThen(Action::DismissProgress);
                               return;
                           }
                         });
}

void TotoSyncActivity::offerRemotePosition() {
  std::array<char, 128> body{};
  std::snprintf(body.data(), body.size(), tr(STR_TOTO_RESUME_BODY), progressDecision->percentage);
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TOTO_RESUME_HEADING), body.data()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          // Backing out decides nothing: the suggestion stays in the inbox and
          // Advanced still offers to drop it.
          resultText = tr(STR_TOTO_RESUME_LATER);
          requestUpdate();
          return;
        }
        ensureWifiThen(Action::AcceptProgress);
      });
}

void TotoSyncActivity::askUsername() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TOTO_USERNAME),
                                                                 TOTO_CREDENTIALS.getAccountUsername(),
                                                                 MAX_CREDENTIAL_LENGTH, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           pendingIdentifier = std::get<KeyboardResult>(result.data).text;
                           if (pendingIdentifier.empty()) {
                             resultText = tr(STR_TOTO_STEP_CREDENTIALS);
                             requestUpdate();
                             return;
                           }
                           askPassword();
                         });
}

void TotoSyncActivity::askPassword() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TOTO_PASSWORD), "",
                                                                 MAX_CREDENTIAL_LENGTH, InputType::Password),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             forgetPendingCredentials();
                             return;
                           }
                           pendingPassword = std::get<KeyboardResult>(result.data).text;
                           if (pendingPassword.empty()) {
                             forgetPendingCredentials();
                             resultText = tr(STR_TOTO_STEP_CREDENTIALS);
                             requestUpdate();
                             return;
                           }
                           ensureWifiThen(Action::SignIn);
                         });
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
                             forgetPendingCredentials();
                             resultText = tr(STR_TOTO_WIFI_REQUIRED);
                             requestUpdate();
                             return;
                           }
                           working = true;
                           requestUpdate();
                         });
}

void TotoSyncActivity::performSignIn() {
  const toto::LoginClient::Outcome outcome = toto::LoginClient::signIn(pendingIdentifier, pendingPassword);
  forgetPendingCredentials();

  if (outcome.result != toto::LoginClient::Result::OK) {
    resultText = nextStepText(outcome.nextStep);
    return;
  }

  pendingSession = outcome.session;
  if (outcome.transition == toto::AccountTransition::SWITCH_NEEDS_DECISION) {
    // Asked, not assumed: the events already queued cannot be delivered to the
    // new account, and throwing away somebody's highlights is their call.
    confirmAccountSwitch(TOTO_QUEUE.depth());
    return;
  }
  applySession(outcome.transition != toto::AccountTransition::SAME_ACCOUNT);
}

void TotoSyncActivity::confirmAccountSwitch(size_t pendingEvents) {
  // The account being left is already on the screen behind this popup, and a
  // code-paired reader has no name to show, so the sentence only counts events.
  std::array<char, 128> body{};
  std::snprintf(body.data(), body.size(), tr(STR_TOTO_SWITCH_BODY), static_cast<unsigned>(pendingEvents));
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TOTO_SWITCH_HEADING), body.data()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          // The new credential is simply dropped; the old
          // session and its queue stay exactly as they were.
          pendingSession = {};
          resultText = tr(STR_TOTO_SWITCH_KEPT);
          requestUpdate();
          return;
        }
        applySession(true);
        requestUpdate();
      });
}

void TotoSyncActivity::applySession(bool discardPreviousAccount) {
  const bool committed = toto::LoginClient::commit(pendingSession, discardPreviousAccount);
  pendingSession = {};
  resultText = committed ? tr(STR_TOTO_SIGNIN_SUCCESS) : tr(STR_TOTO_FAILED);
  refreshSnapshot();
}

void TotoSyncActivity::forgetPendingCredentials() {
  wipe(pendingIdentifier);
  wipe(pendingPassword);
}

void TotoSyncActivity::performPendingAction() {
  switch (pendingAction) {
    case Action::SignIn:
      performSignIn();
      break;
    case Action::RepairServices:
      resultText = toto::bootstrapCrossPointServices() ? tr(STR_TOTO_PAIR_SUCCESS) : tr(STR_TOTO_FAILED);
      break;
    case Action::PairOrClaim: {
      const auto result =
          TOTO_CREDENTIALS.pairingPending() ? toto::PairingClient::pollAndClaim() : toto::PairingClient::request();
      if (result == toto::PairingClient::Result::OK || result == toto::PairingClient::Result::PENDING) {
        resultText = tr(STR_TOTO_PAIR_PENDING);
      } else if (result == toto::PairingClient::Result::PAIRED) {
        resultText = tr(STR_TOTO_PAIR_SUCCESS);
      } else {
        resultText = std::string(tr(STR_TOTO_FAILED)) + ": " + toto::PairingClient::resultName(result);
      }
      break;
    }
    case Action::Sync: {
      const toto::SyncClient::Outcome outcome = toto::SyncClient::syncOnce();
      if (outcome.result == toto::SyncClient::Result::OK) {
        std::array<char, 96> text{};
        std::snprintf(text.data(), text.size(), tr(STR_TOTO_SYNC_RESULT), static_cast<unsigned>(outcome.acknowledged),
                      static_cast<unsigned>(outcome.pulled));
        resultText = text.data();
      } else if (outcome.result == toto::SyncClient::Result::AUTH_ERROR) {
        // The hub refused the device credential: revoked from the web, or spent.
        resultText = nextStepText(toto::NextStep::RELINK_FROM_WEB);
      } else if (outcome.result == toto::SyncClient::Result::NETWORK_ERROR) {
        resultText = nextStepText(toto::NextStep::RECONNECT);
      } else {
        resultText = std::string(tr(STR_TOTO_FAILED)) + ": " + toto::SyncClient::resultName(outcome.result);
      }
      break;
    }
    case Action::AcceptProgress:
    case Action::DismissProgress: {
      if (!progressDecision) break;
      const bool accept = pendingAction == Action::AcceptProgress;
      const auto result = toto::SyncClient::resolveSuggestion(progressDecision->suggestionId, accept);
      if (result == toto::SyncClient::Result::OK) {
        const bool stored = accept ? TOTO_QUEUE.acceptProgress(*progressDecision)
                                   : TOTO_QUEUE.resolveProgress(progressDecision->serverSequence);
        if (!stored) {
          resultText = tr(STR_TOTO_FAILED);
        } else if (accept) {
          resultText = tr(STR_TOTO_RESUME_ACCEPTED);
        } else {
          resultText = tr(STR_TOTO_RESUME_DISMISSED);
        }
      } else {
        resultText = std::string(tr(STR_TOTO_FAILED)) + ": " + toto::SyncClient::resultName(result);
      }
      break;
    }
  }
  refreshSnapshot();
  working = false;
  requestUpdate();
}

const char* TotoSyncActivity::nextStepText(toto::NextStep step) {
  switch (step) {
    case toto::NextStep::REENTER_CREDENTIALS:
      return tr(STR_TOTO_STEP_CREDENTIALS);
    case toto::NextStep::VERIFY_EMAIL:
      return tr(STR_TOTO_STEP_VERIFY_EMAIL);
    case toto::NextStep::RELINK_FROM_WEB:
      return tr(STR_TOTO_STEP_RELINK);
    case toto::NextStep::USE_HTTPS:
      return tr(STR_TOTO_STEP_HTTPS);
    case toto::NextStep::FREE_A_DEVICE_SLOT:
      return tr(STR_TOTO_STEP_DEVICE_LIMIT);
    case toto::NextStep::FIX_REQUEST:
      return tr(STR_TOTO_STEP_FIX_REQUEST);
    case toto::NextStep::RECONNECT:
      return tr(STR_TOTO_STEP_RECONNECT);
    case toto::NextStep::WAIT_AND_RETRY:
    case toto::NextStep::NONE:
      break;
  }
  return tr(STR_TOTO_STEP_WAIT);
}

std::string TotoSyncActivity::rowTitle(int index) const {
  switch (static_cast<toto::MenuRow>(index)) {
    case toto::MenuRow::Account:
      return snapshot.signedIn ? tr(STR_TOTO_MENU_ACCOUNT) : tr(STR_TOTO_SIGN_IN);
    case toto::MenuRow::SyncNow:
      return tr(STR_TOTO_SYNC_NOW);
    case toto::MenuRow::Library:
      return tr(STR_TOTO_MENU_LIBRARY);
    case toto::MenuRow::StatusHelp:
      return tr(STR_TOTO_MENU_STATUS);
    case toto::MenuRow::Advanced:
      break;
  }
  return tr(STR_TOTO_MENU_ADVANCED);
}

std::string TotoSyncActivity::statusLine() const {
  if (!resultText.empty()) return resultText;
  return toto_ui::statusSentence(snapshot);
}

void TotoSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int textWidth = width - metrics.contentSidePadding * 2;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TOTO_SYNC));

  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const std::string account =
      renderer.truncatedText(UI_12_FONT_ID, toto_ui::accountSentence().c_str(), textWidth, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 14, account.c_str(), true, EpdFontFamily::BOLD);

  const std::string status = renderer.truncatedText(UI_10_FONT_ID, statusLine().c_str(), textWidth);
  renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 40, status.c_str());

  if (snapshot.pairingPending) {
    std::array<char, 96> codeLine{};
    std::snprintf(codeLine.data(), codeLine.size(), tr(STR_TOTO_PAIR_CODE),
                  TOTO_CREDENTIALS.getPairingUserCode().c_str());
    renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 64, codeLine.data(), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 64, toto_ui::lastSyncSentence(snapshot).c_str());
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + 86;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, toto::MENU_ROW_COUNT, selectedIndex,
      [this](int index) { return rowTitle(index); }, nullptr, nullptr, nullptr, false,
      [this](int index) { return !toto::rowEnabled(static_cast<toto::MenuRow>(index), snapshot); });

  if (working) {
    GUI.drawPopup(renderer, tr(STR_TOTO_WORKING));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
