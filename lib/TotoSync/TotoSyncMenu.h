#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "TotoDeviceLogin.h"

// Decision core behind the Toto screens. It holds no Arduino, i18n or storage
// dependency on purpose: the activities read the live singletons, hand the
// result over as a snapshot, and render whatever this returns. That is what
// makes the menu rules testable on the host, where no reader exists.
namespace toto {

// Rows of the top-level Toto screen, in screen order. Every capability appears
// exactly once: a reader never has to guess which of two entries is the real
// way in. Diagnosis and recovery live one level down, behind Advanced.
enum class MenuRow : uint8_t {
  Account,
  SyncNow,
  Library,
  StatusHelp,
  Advanced,
};
inline constexpr int MENU_ROW_COUNT = 5;

// Rows of the Advanced screen. Nothing here is part of the normal path.
enum class AdvancedRow : uint8_t {
  PairWithCode,
  RepairServices,
  DiscardRemotePosition,
};
inline constexpr int ADVANCED_ROW_COUNT = 3;

// The single sentence the screen shows about where sync stands, ordered by
// what blocks the reader first. The activity maps it to a translated string.
enum class StatusKind : uint8_t {
  SignedOut,
  PairingPending,
  SessionExpired,
  SessionRenewDue,
  RemotePositionWaiting,
  PendingUploads,
  UpToDate,
};

// Age of the last successful sync, already bucketed so the caller only picks a
// sentence instead of doing calendar arithmetic on an e-reader.
enum class AgeUnit : uint8_t { Never, Moments, Minutes, Hours, Days };

struct Age {
  AgeUnit unit = AgeUnit::Never;
  uint32_t value = 0;

  bool operator==(const Age&) const = default;
};

// Everything the screens need to decide what to show, read once per render.
struct SyncSnapshot {
  bool signedIn = false;
  bool pairingPending = false;
  bool remotePositionWaiting = false;
  size_t outbox = 0;
  size_t inbox = 0;
  uint64_t lastSuccessAt = 0;
  uint64_t now = 0;
  SessionState session = SessionState::SIGNED_OUT;
};

StatusKind summarize(const SyncSnapshot& snapshot);
Age lastSuccessAge(const SyncSnapshot& snapshot);
bool rowEnabled(MenuRow row, const SyncSnapshot& snapshot);
bool advancedRowEnabled(AdvancedRow row, const SyncSnapshot& snapshot);

// The line a reader copies into a problem report. It is built from the
// firmware version and the device id only -- the session token is not a
// parameter here, so no caller can leak it into something shown on screen.
std::string reportCode(std::string_view firmwareVersion, std::string_view deviceId);

// Host of the account service, for the "report a problem at ..." line. Takes
// the configured base URL and drops the scheme so the sentence stays short.
std::string supportHost(std::string_view baseUrl);

}  // namespace toto
