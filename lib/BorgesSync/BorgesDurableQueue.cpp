#include "BorgesDurableQueue.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace borges {
namespace {

constexpr char ROOT_DIR[] = "/.borges/borges";
constexpr char OUTBOX_DIR[] = "/.borges/borges/outbox";
constexpr char INBOX_DIR[] = "/.borges/borges/inbox";
constexpr char CHECKPOINT_A[] = "/.borges/borges/checkpoint-a.bin";
constexpr char CHECKPOINT_B[] = "/.borges/borges/checkpoint-b.bin";

bool validBookHash(std::string_view hash) {
  return hash.size() == 32 &&
         std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string recoveryFilename(std::string_view bookHash) { return "recovery_" + std::string(bookHash) + ".json"; }

std::vector<uint8_t> readCheckpointFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("BORGES", path, file) || file.fileSize() != CHECKPOINT_SIZE) return {};
  std::vector<uint8_t> bytes(CHECKPOINT_SIZE);
  if (file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) return {};
  return bytes;
}

std::string joinPath(const char* directory, const String& filename) {
  return std::string(directory) + "/" + filename.c_str();
}

void recoverAtomic(const std::string& finalPath) {
  const std::string backupPath = finalPath + ".bak";
  if (Storage.exists(backupPath.c_str())) {
    if (Storage.exists(finalPath.c_str())) {
      Storage.remove(backupPath.c_str());
    } else {
      Storage.rename(backupPath.c_str(), finalPath.c_str());
    }
  }
  Storage.remove((finalPath + ".tmp").c_str());
}

std::optional<uint64_t> decimalSequence(const char* value) {
  if (value == nullptr || *value == '\0') return std::nullopt;
  uint64_t parsed = 0;
  const char* end = value;
  while (*end != '\0') ++end;
  const auto result = std::from_chars(value, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return parsed;
}

}  // namespace

DurableQueue& DurableQueue::instance() {
  static DurableQueue queue;
  return queue;
}

bool DurableQueue::begin() {
  if (initialized) return true;
  if (!Storage.ensureDirectoryExists(ROOT_DIR) || !Storage.ensureDirectoryExists(OUTBOX_DIR) ||
      !Storage.ensureDirectoryExists(INBOX_DIR)) {
    LOG_ERR("BORGES", "Could not create durable sync directories");
    return false;
  }

  for (const String& filename : Storage.listFiles(OUTBOX_DIR, MAX_QUEUE_SCAN)) {
    if (filename.endsWith(".bak")) {
      const std::string backup = joinPath(OUTBOX_DIR, filename);
      recoverAtomic(backup.substr(0, backup.size() - 4));
    }
  }
  for (const String& filename : Storage.listFiles(OUTBOX_DIR, MAX_QUEUE_SCAN)) {
    if (filename.endsWith(".tmp")) Storage.remove(joinPath(OUTBOX_DIR, filename).c_str());
  }
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    if (filename.endsWith(".bak")) {
      const std::string backup = joinPath(INBOX_DIR, filename);
      recoverAtomic(backup.substr(0, backup.size() - 4));
    }
  }
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    if (filename.endsWith(".tmp")) Storage.remove(joinPath(INBOX_DIR, filename).c_str());
  }
  recoverAtomic(CHECKPOINT_A);
  recoverAtomic(CHECKPOINT_B);

  if (!loadCheckpoint()) {
    state = {};
    if (!saveCheckpoint()) return false;
  }
  runtimeBootId = esp_random();
  if (runtimeBootId == 0) runtimeBootId = 1;
  initialized = true;
  LOG_INF("BORGES", "Durable queue ready: next=%llu cursor=%llu depth=%u",
          static_cast<unsigned long long>(state.nextSequence), static_cast<unsigned long long>(state.appliedCursor),
          static_cast<unsigned>(depth()));
  return true;
}

std::optional<EventIdentity> DurableQueue::reserveIdentity(std::string preferredId) {
  if (!initialized && !begin()) return std::nullopt;
  if (state.nextSequence == 0 || state.nextSequence == static_cast<uint64_t>(INT64_MAX)) {
    LOG_ERR("BORGES", "Client sequence exhausted");
    return std::nullopt;
  }

  if (preferredId.empty()) {
    std::array<uint8_t, 16> random{};
    esp_fill_random(random.data(), random.size());
    preferredId = formatUuidV4(random);
  } else if (!isUuid(preferredId)) {
    return std::nullopt;
  }
  EventIdentity identity{.id = std::move(preferredId), .sequence = state.nextSequence};
  ++state.nextSequence;
  if (!saveCheckpoint()) {
    --state.nextSequence;
    return std::nullopt;
  }
  return identity;
}

bool DurableQueue::enqueue(const EventIdentity& identity, const std::string& immutableEventJson) {
  if ((!initialized && !begin()) || !isUuid(identity.id) || identity.sequence == 0 || immutableEventJson.empty() ||
      immutableEventJson.size() > MAX_EVENT_BYTES) {
    LOG_ERR("BORGES", "Refused invalid/oversized outbox event");
    return false;
  }
  const std::string path = std::string(OUTBOX_DIR) + "/" + eventFilename(identity.sequence, identity.id, false);
  if (Storage.exists(path.c_str())) return true;
  if (!writeAtomic(path, reinterpret_cast<const uint8_t*>(immutableEventJson.data()), immutableEventJson.size())) {
    LOG_ERR("BORGES", "Could not persist event %s", identity.id.c_str());
    return false;
  }
  return true;
}

bool DurableQueue::replaceUnattempted(const EventIdentity& identity, const std::string& eventJson) {
  if ((!initialized && !begin()) || !isUuid(identity.id) || identity.sequence == 0 || eventJson.empty() ||
      eventJson.size() > MAX_EVENT_BYTES) {
    return false;
  }
  const std::string attempted = std::string(OUTBOX_DIR) + "/" + eventFilename(identity.sequence, identity.id, true);
  if (Storage.exists(attempted.c_str())) return false;
  const std::string pending = std::string(OUTBOX_DIR) + "/" + eventFilename(identity.sequence, identity.id, false);
  if (!Storage.exists(pending.c_str())) return false;
  return writeAtomic(pending, reinterpret_cast<const uint8_t*>(eventJson.data()), eventJson.size());
}

std::vector<PendingEvent> DurableQueue::nextBatch(size_t maxEvents, size_t maxBytes) const {
  maxEvents = std::min(maxEvents, MAX_BATCH_EVENTS);
  maxBytes = std::min(maxBytes, MAX_BATCH_BYTES);
  std::vector<std::pair<uint64_t, String>> ordered;
  ordered.reserve(MAX_QUEUE_SCAN);

  for (const String& filename : Storage.listFiles(OUTBOX_DIR, MAX_QUEUE_SCAN)) {
    uint64_t sequence = 0;
    std::string id;
    bool attempted = false;
    if (parseFilename(filename.c_str(), sequence, id, attempted)) ordered.emplace_back(sequence, filename);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });

  std::vector<PendingEvent> batch;
  batch.reserve(maxEvents);
  size_t used = 0;
  for (const auto& [sequence, filename] : ordered) {
    if (batch.size() >= maxEvents) break;
    std::string id;
    uint64_t parsedSequence = 0;
    bool attempted = false;
    if (!parseFilename(filename.c_str(), parsedSequence, id, attempted)) continue;
    const std::string path = joinPath(OUTBOX_DIR, filename);
    const String body = Storage.readFile(path.c_str());
    if (body.isEmpty() || body.length() > MAX_EVENT_BYTES) continue;
    if (!batch.empty() && used + body.length() > maxBytes) break;
    used += body.length();
    batch.push_back({.id = std::move(id), .sequence = sequence, .body = body.c_str(), .attempted = attempted});
  }
  return batch;
}

bool DurableQueue::markAttempted(const PendingEvent& event) {
  if (event.attempted || !isUuid(event.id) || event.sequence == 0) return event.attempted;
  const std::string source = std::string(OUTBOX_DIR) + "/" + eventFilename(event.sequence, event.id, false);
  const std::string target = std::string(OUTBOX_DIR) + "/" + eventFilename(event.sequence, event.id, true);
  if (Storage.exists(target.c_str())) return true;
  return Storage.rename(source.c_str(), target.c_str());
}

bool DurableQueue::retireAcknowledged(std::string_view eventId, uint64_t sequence) {
  if (!isUuid(eventId) || sequence == 0) return false;
  const std::string pending = std::string(OUTBOX_DIR) + "/" + eventFilename(sequence, eventId, false);
  const std::string attempted = std::string(OUTBOX_DIR) + "/" + eventFilename(sequence, eventId, true);
  if (Storage.exists(attempted.c_str())) return Storage.remove(attempted.c_str());
  if (Storage.exists(pending.c_str())) return Storage.remove(pending.c_str());
  return true;
}

bool DurableQueue::deferPulled(uint64_t serverSequence, const std::string& eventJson) {
  if ((!initialized && !begin()) || serverSequence == 0 || eventJson.empty() ||
      eventJson.size() > MAX_INBOX_EVENT_BYTES) {
    return false;
  }
  std::array<char, 40> filename{};
  std::snprintf(filename.data(), filename.size(), "%020llu.json", static_cast<unsigned long long>(serverSequence));
  const std::string path = std::string(INBOX_DIR) + "/" + filename.data();
  std::string acceptedPath = path;
  acceptedPath.replace(acceptedPath.size() - 5, 5, ".accepted");
  if (Storage.exists(acceptedPath.c_str())) return true;
  std::string dismissedPath = path;
  dismissedPath.replace(dismissedPath.size() - 5, 5, ".dismissed");
  if (Storage.exists(dismissedPath.c_str())) return true;
  if (Storage.exists(path.c_str())) return true;
  return writeAtomic(path, reinterpret_cast<const uint8_t*>(eventJson.data()), eventJson.size());
}

bool DurableQueue::restoreProgressCandidate(std::string_view bookHash, const std::string& eventJson) {
  if ((!initialized && !begin()) || !validBookHash(bookHash) || eventJson.empty() ||
      eventJson.size() > MAX_INBOX_EVENT_BYTES)
    return false;
  JsonDocument event;
  if (deserializeJson(event, eventJson) || !event.is<JsonObject>() ||
      std::string(event["event_type"] | "") != "progress.changed" ||
      std::string(event["book_identifier"]["kind"] | "") != "koreader_partial_md5" ||
      std::string(event["book_identifier"]["value"] | "") != bookHash)
    return false;
  const auto source = event["directive_metadata"]["source"].as<JsonObjectConst>();
  const auto payload = event["payload"].as<JsonObjectConst>();
  const auto percentage = source["percentage"].isNull() ? payload["percentage"] : source["percentage"];
  if (!percentage.is<double>() || !std::isfinite(percentage.as<double>()) || percentage.as<double>() < 0 ||
      percentage.as<double>() > 100)
    return false;

  std::array<uint8_t, 16> random{};
  esp_fill_random(random.data(), random.size());
  JsonObject local = event["local_recovery"].to<JsonObject>();
  local["request_id"] = formatUuidV4(random);
  local["decision"] = "pending";
  // A deliberate recovery request must be offered even when the stream once
  // classified the same position as own, equivalent, or already dismissed.
  event["directive"] = "suggest_resume";
  std::string serialized;
  serializeJson(event, serialized);
  if (event.overflowed() || serialized.size() > MAX_INBOX_EVENT_BYTES) return false;
  const std::string path = std::string(INBOX_DIR) + "/" + recoveryFilename(bookHash);
  return writeAtomic(path, reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
}

std::optional<ProgressInboxItem> DurableQueue::nextProgressDecision(std::string_view bookHash,
                                                                    bool includeDismissed) const {
  uint64_t acceptedByManual = 0;
  if (validBookHash(bookHash)) {
    if (const auto manual = readProgressInbox(recoveryFilename(bookHash))) {
      if (manual->accepted) return std::nullopt;
      if (!manual->dismissed || includeDismissed) return manual;
      acceptedByManual = manual->supersedingStreamSequence;
    }
  }
  std::optional<ProgressInboxItem> newest;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    const auto item = readProgressInbox(filename.c_str());
    if (!item || item->accepted || (acceptedByManual != 0 && item->serverSequence == acceptedByManual) ||
        (!bookHash.empty() && item->bookHash != bookHash) || (item->dismissed && !includeDismissed) ||
        item->directive == "own" || item->directive == "equivalent" || item->directive == "keep_local")
      continue;
    if (!newest || (item->manualRecovery && !newest->manualRecovery) ||
        (item->manualRecovery == newest->manualRecovery && item->serverSequence > newest->serverSequence))
      newest = item;
  }
  return newest;
}

std::optional<ProgressInboxItem> DurableQueue::nextApplicableProgress(std::string_view bookHash) const {
  if (validBookHash(bookHash)) {
    if (const auto manual = readProgressInbox(recoveryFilename(bookHash))) {
      // A new manual request supersedes old consent for this book. It must not
      // accidentally activate an older .accepted stream entry before review.
      if (manual->accepted) return manual;
      if (manual->supersedingStreamSequence != 0) {
        auto chosen = readStreamProgress(manual->supersedingStreamSequence);
        if (chosen && chosen->bookHash == bookHash) {
          chosen->accepted = true;
          return chosen;
        }
      }
      return std::nullopt;
    }
  }
  std::optional<ProgressInboxItem> newest;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    const auto item = readProgressInbox(filename.c_str());
    if (!item || item->bookHash != bookHash) continue;
    const bool safe = item->accepted || shouldAutoApply(parseProgressDirective(item->directive));
    if (safe && (!newest || item->serverSequence > newest->serverSequence)) newest = item;
  }
  return newest;
}

std::optional<AnnotationInboxItem> DurableQueue::nextAnnotation(std::string_view bookHash,
                                                                uint64_t afterSequence) const {
  std::optional<AnnotationInboxItem> oldest;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    const auto item = readAnnotationInbox(filename.c_str());
    if (!item || item->bookHash != bookHash || item->serverSequence <= afterSequence) continue;
    if (!oldest || item->serverSequence < oldest->serverSequence) oldest = item;
  }
  return oldest;
}

bool DurableQueue::acceptProgress(const ProgressInboxItem& item) {
  if (item.manualRecovery) {
    if (!validBookHash(item.bookHash)) return false;
    const auto stored = readProgressInbox(recoveryFilename(item.bookHash));
    if (!stored || stored->recoveryRequestId != item.recoveryRequestId) return false;
    return setRecoveryDecision(item, "accepted");
  }
  if (validBookHash(item.bookHash)) {
    if (const auto manual = readProgressInbox(recoveryFilename(item.bookHash))) {
      const auto chosen = readStreamProgress(item.serverSequence);
      if (!chosen || chosen->bookHash != item.bookHash) return false;
      // Consent and the exact chosen stream sequence commit in one file. Never
      // reactivate unrelated old .accepted entries hidden beyond a directory scan.
      return setRecoveryDecision(*manual, "superseded", item.serverSequence);
    }
  }
  const uint64_t serverSequence = item.serverSequence;
  std::array<char, 72> source{};
  std::array<char, 72> target{};
  std::snprintf(source.data(), source.size(), "%s/%020llu.json", INBOX_DIR,
                static_cast<unsigned long long>(serverSequence));
  std::snprintf(target.data(), target.size(), "%s/%020llu.accepted", INBOX_DIR,
                static_cast<unsigned long long>(serverSequence));
  if (item.dismissed) {
    std::snprintf(source.data(), source.size(), "%s/%020llu.dismissed", INBOX_DIR,
                  static_cast<unsigned long long>(serverSequence));
  }
  const bool accepted = Storage.exists(target.data()) || Storage.rename(source.data(), target.data());
  if (!accepted) return false;

  bool removed = true;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    const auto candidate = readProgressInbox(filename.c_str());
    if (!candidate || candidate->manualRecovery || candidate->bookHash != item.bookHash ||
        candidate->serverSequence >= serverSequence)
      continue;
    removed = removeInboxFile(candidate->serverSequence) && removed;
  }
  return removed;
}

bool DurableQueue::dismissProgress(const ProgressInboxItem& item) {
  if (item.manualRecovery) {
    if (!validBookHash(item.bookHash)) return false;
    const auto stored = readProgressInbox(recoveryFilename(item.bookHash));
    if (!stored || stored->recoveryRequestId != item.recoveryRequestId) return false;
    return setRecoveryDecision(item, "dismissed");
  }
  std::array<char, 72> source{}, target{};
  std::snprintf(source.data(), source.size(), "%s/%020llu.json", INBOX_DIR,
                static_cast<unsigned long long>(item.serverSequence));
  if (item.accepted) {
    std::snprintf(source.data(), source.size(), "%s/%020llu.accepted", INBOX_DIR,
                  static_cast<unsigned long long>(item.serverSequence));
  }
  std::snprintf(target.data(), target.size(), "%s/%020llu.dismissed", INBOX_DIR,
                static_cast<unsigned long long>(item.serverSequence));
  if (Storage.exists(target.data())) {
    return !Storage.exists(source.data()) || Storage.remove(source.data());
  }
  return Storage.rename(source.data(), target.data());
}

std::optional<ProgressInboxItem> DurableQueue::readStreamProgress(uint64_t serverSequence) {
  if (serverSequence == 0) return std::nullopt;
  for (const char* suffix : {"accepted", "json", "dismissed"}) {
    std::array<char, 40> filename{};
    std::snprintf(filename.data(), filename.size(), "%020llu.%s", static_cast<unsigned long long>(serverSequence),
                  suffix);
    if (const auto item = readProgressInbox(filename.data())) return item;
  }
  return std::nullopt;
}

bool DurableQueue::resolveProgress(uint64_t serverSequence) { return removeInboxFile(serverSequence); }

bool DurableQueue::setRecoveryDecision(const ProgressInboxItem& item, const char* decision, uint64_t streamSequence) {
  if (!item.manualRecovery || !validBookHash(item.bookHash) || !isUuid(item.recoveryRequestId)) return false;
  const std::string path = std::string(INBOX_DIR) + "/" + recoveryFilename(item.bookHash);
  recoverAtomic(path);
  const String body = Storage.readFile(path.c_str());
  if (body.isEmpty() || body.length() > MAX_INBOX_EVENT_BYTES) return false;
  JsonDocument event;
  if (deserializeJson(event, body.c_str()) ||
      std::string(event["local_recovery"]["request_id"] | "") != item.recoveryRequestId)
    return false;
  event["local_recovery"]["decision"] = decision;
  if (streamSequence != 0)
    event["local_recovery"]["stream_sequence"] = std::to_string(streamSequence);
  else
    event["local_recovery"].remove("stream_sequence");
  std::string serialized;
  serializeJson(event, serialized);
  if (event.overflowed() || serialized.size() > MAX_INBOX_EVENT_BYTES) return false;
  return writeAtomic(path, reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
}

bool DurableQueue::resolveProgress(const ProgressInboxItem& item) {
  if (!item.manualRecovery) {
    if (item.serverSequence == 0 || !validBookHash(item.bookHash)) return false;
    const auto stored = readStreamProgress(item.serverSequence);
    if (stored && stored->bookHash != item.bookHash) return false;
    if (!removeInboxFile(item.serverSequence)) return false;
    return resolveProgressThrough(item.bookHash, item.serverSequence);
  }
  if (!validBookHash(item.bookHash) || !isUuid(item.recoveryRequestId)) return false;
  const auto stored = readProgressInbox(recoveryFilename(item.bookHash));
  if (!stored || stored->recoveryRequestId != item.recoveryRequestId) return false;
  if (stored->recoveryResolved) return true;
  if (!stored->accepted) return false;
  // Keep a durable barrier against old consent, and retain the location for
  // manual recovery. Future stream suggestions still require a fresh choice.
  return setRecoveryDecision(item, "resolved");
}

bool DurableQueue::resolveInbox(uint64_t serverSequence) { return removeInboxFile(serverSequence); }

bool DurableQueue::resolveProgressThrough(std::string_view bookHash, uint64_t serverSequence) {
  bool removed = true;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    const auto item = readProgressInbox(filename.c_str());
    if (!item || item->manualRecovery || item->bookHash != bookHash || item->serverSequence > serverSequence) continue;
    removed = removeInboxFile(item->serverSequence) && removed;
  }
  return removed;
}

bool DurableQueue::removeInboxFile(uint64_t serverSequence) {
  std::array<char, 72> filename{};
  std::snprintf(filename.data(), filename.size(), "%s/%020llu.json", INBOX_DIR,
                static_cast<unsigned long long>(serverSequence));
  bool removed = !Storage.exists(filename.data()) || Storage.remove(filename.data());
  std::snprintf(filename.data(), filename.size(), "%s/%020llu.accepted", INBOX_DIR,
                static_cast<unsigned long long>(serverSequence));
  return (!Storage.exists(filename.data()) || Storage.remove(filename.data())) && removed;
}

size_t DurableQueue::depth() const {
  size_t count = 0;
  for (const String& filename : Storage.listFiles(OUTBOX_DIR, MAX_QUEUE_SCAN)) {
    uint64_t sequence = 0;
    std::string id;
    bool attempted = false;
    if (parseFilename(filename.c_str(), sequence, id, attempted)) ++count;
  }
  return count;
}

size_t DurableQueue::inboxDepth() const {
  size_t count = 0;
  for (const String& filename : Storage.listFiles(INBOX_DIR, MAX_QUEUE_SCAN)) {
    if (filename.endsWith(".json") || filename.endsWith(".accepted")) ++count;
  }
  return count;
}

bool DurableQueue::commitAppliedCursor(uint64_t cursor) {
  if (cursor < state.appliedCursor) return false;
  if (cursor == state.appliedCursor) return true;
  const uint64_t previous = state.appliedCursor;
  state.appliedCursor = cursor;
  if (saveCheckpoint()) return true;
  state.appliedCursor = previous;
  return false;
}

bool DurableQueue::setWallAnchor(uint64_t unixSeconds, uint32_t monotonicMs) {
  if (unixSeconds == 0) return false;
  const Checkpoint previous = state;
  state.wallAnchorUnixSeconds = unixSeconds;
  state.monotonicAnchorMs = monotonicMs;
  state.bootId = runtimeBootId;
  state.anchorPrecision = TimePrecision::EXACT;
  if (saveCheckpoint()) return true;
  state = previous;
  return false;
}

bool DurableQueue::loadCheckpoint() {
  const auto selected = selectNewestCheckpoint(readCheckpointFile(CHECKPOINT_A), readCheckpointFile(CHECKPOINT_B));
  if (!selected) return false;
  state = *selected;
  return true;
}

bool DurableQueue::saveCheckpoint() {
  Checkpoint next = state;
  ++next.generation;
  const auto bytes = encodeCheckpoint(next);
  const char* target = (next.generation & 1U) == 0 ? CHECKPOINT_A : CHECKPOINT_B;
  if (!writeAtomic(target, bytes.data(), bytes.size())) {
    LOG_ERR("BORGES", "Checkpoint commit failed");
    return false;
  }
  state = next;
  return true;
}

std::string DurableQueue::eventFilename(uint64_t sequence, std::string_view id, bool attempted) {
  std::array<char, 80> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%020llu-%.*s.%s", static_cast<unsigned long long>(sequence),
                static_cast<int>(id.size()), id.data(), attempted ? "sent" : "pending");
  return buffer.data();
}

bool DurableQueue::parseFilename(std::string_view filename, uint64_t& sequence, std::string& id, bool& attempted) {
  constexpr size_t PREFIX_LENGTH = 20;
  constexpr size_t UUID_OFFSET = PREFIX_LENGTH + 1;
  constexpr size_t UUID_LENGTH = 36;
  if (filename.size() < UUID_OFFSET + UUID_LENGTH + 5 || filename[PREFIX_LENGTH] != '-') return false;
  const auto parsed = std::from_chars(filename.data(), filename.data() + PREFIX_LENGTH, sequence);
  if (parsed.ec != std::errc{} || parsed.ptr != filename.data() + PREFIX_LENGTH || sequence == 0) return false;
  id.assign(filename.substr(UUID_OFFSET, UUID_LENGTH));
  if (!isUuid(id)) return false;
  const std::string_view suffix = filename.substr(UUID_OFFSET + UUID_LENGTH);
  if (suffix == ".pending") {
    attempted = false;
  } else if (suffix == ".sent") {
    attempted = true;
  } else {
    return false;
  }
  return true;
}

std::optional<ProgressInboxItem> DurableQueue::readProgressInbox(std::string_view filename) {
  const bool manual = filename.starts_with("recovery_") && filename.ends_with(".json") && filename.size() == 46 &&
                      validBookHash(filename.substr(9, 32));
  const bool accepted = filename.ends_with(".accepted");
  const bool dismissed = filename.ends_with(".dismissed");
  if (!accepted && !dismissed && !filename.ends_with(".json")) return std::nullopt;
  if (filename.size() < 20) return std::nullopt;
  uint64_t serverSequence = 0;
  if (!manual) {
    const auto parsed = std::from_chars(filename.data(), filename.data() + 20, serverSequence);
    if (parsed.ec != std::errc{} || parsed.ptr != filename.data() + 20 || serverSequence == 0) return std::nullopt;
  }

  const std::string path = std::string(INBOX_DIR) + "/" + std::string(filename);
  // Direct lookup must also recover a manual record when an interrupted
  // replacement left only its backup, even beyond the bounded directory scan.
  const String body = Storage.readFile((manual && !Storage.exists(path.c_str()) ? path + ".bak" : path).c_str());
  if (body.isEmpty() || body.length() > MAX_INBOX_EVENT_BYTES) return std::nullopt;
  JsonDocument event;
  if (deserializeJson(event, body.c_str()) || std::string(event["event_type"] | "") != "progress.changed") {
    return std::nullopt;
  }

  ProgressInboxItem item;
  item.serverSequence = serverSequence;
  item.directive = event["directive"] | "";
  item.suggestionId = event["directive_metadata"]["suggestion"]["id"] | "";
  item.sourceDeviceName = boundedUtf8(event["origin_device"]["name"] | "", 80);
  item.bookHash = event["book_identifier"]["value"] | "";
  const JsonObjectConst source = event["directive_metadata"]["source"].as<JsonObjectConst>();
  const JsonObjectConst payload = event["payload"].as<JsonObjectConst>();
  item.percentage = source["percentage"].isNull() ? (payload["percentage"] | 0.0) : source["percentage"].as<double>();
  item.currentPage =
      source["current_page"].isNull() ? (payload["current_page"] | 0U) : source["current_page"].as<uint32_t>();
  item.totalPages =
      source["total_pages"].isNull() ? (payload["total_pages"] | 0U) : source["total_pages"].as<uint32_t>();
  item.xpointer =
      source["xpointer"].isNull() ? std::string(payload["xpointer"] | "") : source["xpointer"].as<std::string>();
  item.accepted = accepted;
  item.dismissed = dismissed;
  item.manualRecovery = manual;
  if (manual) {
    item.recoveryRequestId = event["local_recovery"]["request_id"] | "";
    const std::string decision = event["local_recovery"]["decision"] | "";
    if (!isUuid(item.recoveryRequestId) || item.bookHash != filename.substr(9, 32) ||
        (decision != "pending" && decision != "accepted" && decision != "dismissed" && decision != "resolved" &&
         decision != "superseded"))
      return std::nullopt;
    item.accepted = decision == "accepted";
    item.dismissed = decision == "dismissed" || decision == "resolved" || decision == "superseded";
    item.recoveryResolved = decision == "resolved";
    if (decision == "superseded") {
      const auto sequence = decimalSequence(event["local_recovery"]["stream_sequence"] | "");
      if (!sequence || *sequence == 0) return std::nullopt;
      item.supersedingStreamSequence = *sequence;
    }
  }
  if (item.bookHash.size() != 32 || !std::isfinite(item.percentage) || item.percentage < 0 || item.percentage > 100)
    return std::nullopt;
  return item;
}

std::optional<AnnotationInboxItem> DurableQueue::readAnnotationInbox(std::string_view filename) {
  if (!filename.ends_with(".json") || filename.size() < 20) return std::nullopt;
  uint64_t serverSequence = 0;
  const auto parsed = std::from_chars(filename.data(), filename.data() + 20, serverSequence);
  if (parsed.ec != std::errc{} || parsed.ptr != filename.data() + 20 || serverSequence == 0) {
    return std::nullopt;
  }

  const std::string path = std::string(INBOX_DIR) + "/" + std::string(filename);
  const String body = Storage.readFile(path.c_str());
  if (body.isEmpty() || body.length() > MAX_INBOX_EVENT_BYTES) return std::nullopt;
  JsonDocument event;
  if (deserializeJson(event, body.c_str())) return std::nullopt;
  const std::string eventType = event["event_type"] | "";
  if (eventType.rfind("annotation.", 0) != 0 && eventType.rfind("bookmark.", 0) != 0) {
    return std::nullopt;
  }

  const JsonObjectConst payload = event["payload"].as<JsonObjectConst>();
  AnnotationInboxItem item;
  item.serverSequence = serverSequence;
  item.directive = event["directive"] | "";
  item.eventType = eventType;
  item.bookHash = event["book_identifier"]["value"] | "";
  item.syncId =
      payload["sync_id"].isNull() ? std::string(event["aggregate_id"] | "") : payload["sync_id"].as<std::string>();
  item.text = boundedUtf8(payload["text"] | "", 3584);
  item.note = boundedUtf8(payload["note"] | "", 768);
  item.xpointer = boundedUtf8(payload["xpointer"] | "", 512);
  item.percentage = payload["percentage"] | 0.0;
  if (payload["percentage"].isNull()) {
    const double page = payload["page"] | 0.0;
    const double totalPages = payload["total_pages"] | 0.0;
    if (totalPages > 0) item.percentage = std::clamp(page / totalPages * 100.0, 0.0, 100.0);
  }
  item.spineIndex = payload["position"]["spine_index"] | static_cast<uint16_t>(0);
  item.chapterPageCount = payload["position"]["chapter_page_count"] | static_cast<uint16_t>(0);
  item.chapterProgress = payload["position"]["chapter_progress"] | static_cast<uint16_t>(0);
  item.deleted = eventType == "annotation.deleted" || eventType == "bookmark.deleted";
  item.conflict =
      item.directive == "annotation_conflict" || (event["directive_metadata"]["annotation_conflict"] | false);
  const char* revision = event["directive_metadata"]["annotation_revision"] | "";
  if (const auto value = decimalSequence(revision)) item.revision = *value;
  if (item.bookHash.size() != 32 || !isUuid(item.syncId) || item.percentage < 0 || item.percentage > 100) {
    return std::nullopt;
  }
  return item;
}

bool DurableQueue::writeAtomic(const std::string& finalPath, const uint8_t* data, size_t size) {
  const std::string tempPath = finalPath + ".tmp";
  const std::string backupPath = finalPath + ".bak";
  recoverAtomic(finalPath);
  Storage.remove(tempPath.c_str());
  {
    HalFile file;
    if (!Storage.openFileForWrite("BORGES", tempPath, file)) return false;
    if (file.write(data, size) != size) return false;
    file.flush();
  }

  const bool hadFinal = Storage.exists(finalPath.c_str());
  if (hadFinal && !Storage.rename(finalPath.c_str(), backupPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (Storage.rename(tempPath.c_str(), finalPath.c_str())) {
    Storage.remove(backupPath.c_str());
    return true;
  }
  if (hadFinal) Storage.rename(backupPath.c_str(), finalPath.c_str());
  Storage.remove(tempPath.c_str());
  return false;
}

}  // namespace borges
