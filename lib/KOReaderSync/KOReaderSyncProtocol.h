#pragma once

#include <string_view>

enum class KOReaderSyncOperation { AUTHENTICATE, CREATE_USER, GET_PROGRESS, PUT_PROGRESS };

enum class KOReaderSyncResponse {
  OK,
  NOT_FOUND,
  AUTH_FAILED,
  ACCESS_DENIED,
  INVALID_REQUEST,
  USER_EXISTS,
  SERVER_ERROR,
};

bool isValidKOReaderDocumentHash(std::string_view value);
int parseKOReaderProtocolCode(std::string_view body);
bool isEmptyKOReaderProgress(std::string_view body);
KOReaderSyncResponse classifyKOReaderResponse(KOReaderSyncOperation operation, int httpCode, std::string_view body);
