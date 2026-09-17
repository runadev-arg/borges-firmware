#include "TotoResumeStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "TotoDurableQueue.h"

namespace toto {
namespace {

constexpr char ROOT_DIR[] = "/.crosspoint/toto";
constexpr char DECISIONS_PATH[] = "/.crosspoint/toto/resume.log";
// Forty books at roughly 150 bytes each, with room for a long anchor. Anything
// past this is a damaged file, not a log.
constexpr size_t MAX_DECISIONS_BYTES = 12 * 1024;

}  // namespace

ResumeStore& ResumeStore::instance() {
  static ResumeStore store;
  return store;
}

void ResumeStore::load() {
  if (loaded) return;
  loaded = true;
  if (!Storage.exists(DECISIONS_PATH)) return;
  const String body = Storage.readFile(DECISIONS_PATH);
  if (body.isEmpty() || body.length() > MAX_DECISIONS_BYTES) {
    LOG_ERR("TOTO", "Ignoring unreadable resume decisions file");
    return;
  }
  log = ResumeDecisionLog::parse(std::string_view(body.c_str(), body.length()));
}

ResumeDecisionLog& ResumeStore::decisions() {
  load();
  return log;
}

bool ResumeStore::save() {
  if (!Storage.ensureDirectoryExists(ROOT_DIR)) return false;
  const std::string text = log.serialize();
  return DurableQueue::writeAtomic(DECISIONS_PATH, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool ResumeStore::remember(std::string_view bookHash, const ResumePosition& position, bool accepted,
                           std::string_view suggestionId, uint64_t at, bool synced) {
  load();
  if (!log.remember(bookHash, position, accepted, suggestionId, at, synced)) return false;
  if (save()) return true;
  // The answer could not be made durable, so it is not kept in RAM either:
  // a reader who is told "done" and asked again after a reboot trusts the
  // screen less than one who is simply asked again now.
  log.forget(bookHash);
  LOG_ERR("TOTO", "Could not persist the resume decision");
  return false;
}

bool ResumeStore::markSynced(std::string_view bookHash, std::string_view suggestionId) {
  load();
  if (!log.markSynced(bookHash, suggestionId)) return false;
  return save();
}

bool ResumeStore::forget(std::string_view bookHash) {
  load();
  if (!log.forget(bookHash)) return false;
  return save();
}

bool ResumeStore::purge() {
  loaded = true;
  log.clear();
  if (!Storage.exists(DECISIONS_PATH)) return true;
  return Storage.remove(DECISIONS_PATH);
}

}  // namespace toto
