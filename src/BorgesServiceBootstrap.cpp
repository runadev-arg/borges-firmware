#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "OpdsServerStore.h"
#include "BorgesCredentialStore.h"

namespace borges {
namespace {

constexpr char LEGACY_OPDS_NAME[] = "Borges Library";

}  // namespace

bool bootstrapBorgesServices() {
  if (!BORGES_CREDENTIALS.paired()) return false;

  const std::string kosyncUrl = BORGES_CREDENTIALS.getBaseUrl() + "/api/kosync";
  const bool koreaderCurrent =
      KOREADER_STORE.getUsername() == BORGES_CREDENTIALS.getDeviceId() &&
      KOREADER_STORE.getPassword() == BORGES_CREDENTIALS.getToken() && KOREADER_STORE.getServerUrl() == kosyncUrl &&
      KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY && KOREADER_STORE.getSendMetadata() &&
      KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::ASK_EVERY_TIME;
  bool koreaderSaved = true;
  if (!koreaderCurrent) {
    KOREADER_STORE.setCredentials(BORGES_CREDENTIALS.getDeviceId(), BORGES_CREDENTIALS.getToken());
    KOREADER_STORE.setServerUrl(kosyncUrl);
    KOREADER_STORE.setMatchMethod(DocumentMatchMethod::BINARY);
    KOREADER_STORE.setSendMetadata(true);
    KOREADER_STORE.setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    koreaderSaved = KOREADER_STORE.saveToFile();
  }

  const OpdsServer server{
      .name = tr(STR_BORGES_LIBRARY),
      .url = BORGES_CREDENTIALS.getBaseUrl() + "/api/opds",
      .username = BORGES_CREDENTIALS.getDeviceId(),
      .password = BORGES_CREDENTIALS.getToken(),
  };
  bool opdsSaved = false;
  for (size_t index = 0; index < OPDS_STORE.getCount(); ++index) {
    const OpdsServer* current = OPDS_STORE.getServer(index);
    if (current != nullptr && (current->name == LEGACY_OPDS_NAME || current->url == server.url)) {
      const bool opdsCurrent = current->name == server.name && current->url == server.url &&
                               current->username == server.username && current->password == server.password;
      opdsSaved = opdsCurrent || OPDS_STORE.updateServer(index, server);
      break;
    }
  }
  if (!opdsSaved) opdsSaved = OPDS_STORE.addServer(server);

  if (!koreaderSaved || !opdsSaved) {
    LOG_ERR("BORGES", "Pairing succeeded but service bootstrap could not be fully persisted");
    return false;
  }
  LOG_INF("BORGES", "Paired services configured for device %s", BORGES_CREDENTIALS.getDeviceId().c_str());
  return true;
}

}  // namespace borges
