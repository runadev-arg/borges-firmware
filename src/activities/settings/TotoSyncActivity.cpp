#include "TotoSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "TotoCredentialStore.h"
#include "TotoDurableQueue.h"
#include "TotoLoginClient.h"
#include "TotoPairingClient.h"
#include "TotoSyncClient.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
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
  resultText.clear();
  refreshDecision();
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
    if (TOTO_CREDENTIALS.paired()) {
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TOTO_SIGN_OUT), accountLine()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            toto::LoginClient::signOut();
            resultText = tr(STR_TOTO_SIGNED_OUT_DONE);
            refreshDecision();
            requestUpdate();
          });
      return;
    }
    askUsername();
  } else if (selectedIndex == 1) {
    ensureWifiThen(Action::PairOrClaim);
  } else if (selectedIndex == 2 && TOTO_CREDENTIALS.paired()) {
    ensureWifiThen(Action::Sync);
  } else if (selectedIndex == 3 && progressDecision) {
    ensureWifiThen(Action::AcceptProgress);
  } else if (selectedIndex == 4 && progressDecision) {
    ensureWifiThen(Action::DismissProgress);
  } else {
    resultText = tr(STR_TOTO_SIGNED_OUT);
    requestUpdate();
  }
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
  refreshDecision();
}

void TotoSyncActivity::forgetPendingCredentials() {
  wipe(pendingIdentifier);
  wipe(pendingPassword);
}

void TotoSyncActivity::performPendingAction() {
  if (pendingAction == Action::SignIn) {
    performSignIn();
  } else if (pendingAction == Action::PairOrClaim) {
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
      }
    }
  } else if (pendingAction == Action::Sync) {
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
  } else if (progressDecision) {
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
  }
  refreshDecision();
  working = false;
  requestUpdate();
}

void TotoSyncActivity::refreshDecision() { progressDecision = TOTO_QUEUE.nextProgressDecision(); }

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

std::string TotoSyncActivity::accountLine() const {
  if (!TOTO_CREDENTIALS.paired()) return tr(STR_TOTO_SIGNED_OUT);
  const std::string& account = TOTO_CREDENTIALS.getAccountUsername();
  // A reader that arrived through code pairing never learned the account name.
  if (account.empty()) return tr(STR_TOTO_PAIRED);
  std::array<char, 128> line{};
  std::snprintf(line.data(), line.size(), tr(STR_TOTO_ACCOUNT), account.c_str());
  return line.data();
}

std::string TotoSyncActivity::statusLine() const {
  if (!resultText.empty()) return resultText;
  switch (TOTO_CREDENTIALS.state(nowUnixSeconds())) {
    case toto::SessionState::EXPIRED:
      return tr(STR_TOTO_SESSION_EXPIRED);
    case toto::SessionState::RENEW_DUE:
      return tr(STR_TOTO_SESSION_RENEW);
    default:
      return {};
  }
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
      renderer.truncatedText(UI_12_FONT_ID, accountLine().c_str(), textWidth, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, summaryTop + 14, account.c_str(), true, EpdFontFamily::BOLD);

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
  } else if (progressDecision && resultText.empty()) {
    std::array<char, 96> resumeLine{};
    std::snprintf(resumeLine.data(), resumeLine.size(), tr(STR_TOTO_RESUME_AT), progressDecision->percentage);
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 64, resumeLine.data(), true, EpdFontFamily::BOLD);
  } else if (const std::string status = statusLine(); !status.empty()) {
    const std::string line = renderer.truncatedText(UI_10_FONT_ID, status.c_str(), textWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, summaryTop + 64, line.c_str());
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + 86;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, MENU_ITEMS, selectedIndex,
      [](int index) {
        if (index == 0) {
          return std::string(TOTO_CREDENTIALS.paired() ? tr(STR_TOTO_SIGN_OUT) : tr(STR_TOTO_SIGN_IN));
        }
        if (index == 1) {
          if (TOTO_CREDENTIALS.paired()) return std::string(tr(STR_TOTO_REPAIR_SERVICES));
          return std::string(TOTO_CREDENTIALS.pairingPending() ? tr(STR_TOTO_CHECK_PAIRING) : tr(STR_TOTO_PAIR_DEVICE));
        }
        if (index == 2) return std::string(tr(STR_TOTO_SYNC_NOW));
        if (index == 3) return std::string(tr(STR_TOTO_ACCEPT_RESUME));
        return std::string(tr(STR_TOTO_DISMISS_RESUME));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 2 && !TOTO_CREDENTIALS.paired()) return std::string("[") + tr(STR_TOTO_SIGNED_OUT) + "]";
        if (index >= 3 && !progressDecision) return std::string("[") + tr(STR_TOTO_NONE) + "]";
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
