#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "OpdsServerStore.h"
#include "TotoCredentialStore.h"

namespace toto {
namespace {

constexpr char LEGACY_OPDS_NAME[] = "Toto Library";

}  // namespace

bool bootstrapCrossPointServices() {
  if (!TOTO_CREDENTIALS.paired()) return false;

  const std::string kosyncUrl = TOTO_CREDENTIALS.getBaseUrl() + "/api/kosync";
  const bool koreaderCurrent =
      KOREADER_STORE.getUsername() == TOTO_CREDENTIALS.getDeviceId() &&
      KOREADER_STORE.getPassword() == TOTO_CREDENTIALS.getToken() && KOREADER_STORE.getServerUrl() == kosyncUrl &&
      KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY && KOREADER_STORE.getSendMetadata() &&
      KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::ASK_EVERY_TIME;
  bool koreaderSaved = true;
  if (!koreaderCurrent) {
    KOREADER_STORE.setCredentials(TOTO_CREDENTIALS.getDeviceId(), TOTO_CREDENTIALS.getToken());
    KOREADER_STORE.setServerUrl(kosyncUrl);
    KOREADER_STORE.setMatchMethod(DocumentMatchMethod::BINARY);
    KOREADER_STORE.setSendMetadata(true);
    KOREADER_STORE.setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    koreaderSaved = KOREADER_STORE.saveToFile();
  }

  const OpdsServer server{
      .name = tr(STR_CINABRIO_LIBRARY),
      .url = TOTO_CREDENTIALS.getBaseUrl() + "/api/opds",
      .username = TOTO_CREDENTIALS.getDeviceId(),
      .password = TOTO_CREDENTIALS.getToken(),
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
    LOG_ERR("TOTO", "Pairing succeeded but service bootstrap could not be fully persisted");
    return false;
  }
  LOG_INF("TOTO", "Paired services configured for device %s", TOTO_CREDENTIALS.getDeviceId().c_str());
  return true;
}

}  // namespace toto
