#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "OpdsServerStore.h"
#include "TotoCredentialStore.h"

namespace toto {
namespace {

constexpr char OPDS_NAME[] = "Toto Library";

}  // namespace

bool bootstrapCrossPointServices() {
  if (!TOTO_CREDENTIALS.paired()) return false;

  KOREADER_STORE.setCredentials(TOTO_CREDENTIALS.getDeviceId(), TOTO_CREDENTIALS.getToken());
  KOREADER_STORE.setServerUrl(TOTO_CREDENTIALS.getBaseUrl() + "/api/kosync");
  KOREADER_STORE.setMatchMethod(DocumentMatchMethod::BINARY);
  KOREADER_STORE.setSendMetadata(true);
  KOREADER_STORE.setSyncBehavior(KOReaderSyncBehavior::SMART);
  const bool koreaderSaved = KOREADER_STORE.saveToFile();

  const OpdsServer server{
      .name = OPDS_NAME,
      .url = TOTO_CREDENTIALS.getBaseUrl() + "/api/opds",
      .username = TOTO_CREDENTIALS.getDeviceId(),
      .password = TOTO_CREDENTIALS.getToken(),
  };
  bool opdsSaved = false;
  for (size_t index = 0; index < OPDS_STORE.getCount(); ++index) {
    const OpdsServer* current = OPDS_STORE.getServer(index);
    if (current != nullptr && (current->name == OPDS_NAME || current->url == server.url)) {
      opdsSaved = OPDS_STORE.updateServer(index, server);
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
