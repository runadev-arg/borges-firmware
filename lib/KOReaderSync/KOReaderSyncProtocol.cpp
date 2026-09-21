#include "KOReaderSyncProtocol.h"

#include <cctype>
#include <limits>

bool isValidKOReaderDocumentHash(std::string_view value) {
  if (value.size() != 32) return false;
  for (const char byte : value) {
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
  }
  return true;
}

int parseKOReaderProtocolCode(std::string_view body) {
  const size_t key = body.find("\"code\"");
  if (key == std::string_view::npos) return 0;
  size_t cursor = body.find(':', key + 6);
  if (cursor == std::string_view::npos) return 0;
  do {
    ++cursor;
  } while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])));
  if (cursor >= body.size() || body[cursor] < '0' || body[cursor] > '9') return 0;
  int result = 0;
  while (cursor < body.size() && body[cursor] >= '0' && body[cursor] <= '9') {
    const int digit = body[cursor++] - '0';
    if (result > (std::numeric_limits<int>::max() - digit) / 10) return 0;
    result = result * 10 + digit;
  }
  return result;
}

bool isEmptyKOReaderProgress(std::string_view body) {
  size_t cursor = 0;
  while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
  if (cursor >= body.size() || body[cursor++] != '{') return false;
  while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
  if (cursor >= body.size() || body[cursor++] != '}') return false;
  while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor;
  return cursor == body.size();
}

KOReaderSyncResponse classifyKOReaderResponse(KOReaderSyncOperation operation, int httpCode, std::string_view body) {
  if (httpCode == 401) return KOReaderSyncResponse::AUTH_FAILED;
  if (operation == KOReaderSyncOperation::CREATE_USER && httpCode == 402) return KOReaderSyncResponse::USER_EXISTS;
  if (operation == KOReaderSyncOperation::GET_PROGRESS &&
      (httpCode == 404 || httpCode == 204 || (httpCode == 200 && isEmptyKOReaderProgress(body)))) {
    return KOReaderSyncResponse::NOT_FOUND;
  }
  if (httpCode >= 200 && httpCode < 300) {
    return KOReaderSyncResponse::OK;
  }
  const int protocolCode = parseKOReaderProtocolCode(body);
  if (httpCode == 400 || httpCode == 404 || httpCode == 413 || protocolCode == 2003 || protocolCode == 2004) {
    return KOReaderSyncResponse::INVALID_REQUEST;
  }
  if (httpCode == 403) return KOReaderSyncResponse::ACCESS_DENIED;
  return KOReaderSyncResponse::SERVER_ERROR;
}
