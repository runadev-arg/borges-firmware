#include <gtest/gtest.h>

#include "KOReaderSyncProtocol.h"

TEST(KOReaderSyncProtocol, ValidatesCanonicalLowercasePartialMd5) {
  EXPECT_TRUE(isValidKOReaderDocumentHash("abcdef0123456789abcdef0123456789"));
  EXPECT_FALSE(isValidKOReaderDocumentHash("ABCDEF0123456789ABCDEF0123456789"));
  EXPECT_FALSE(isValidKOReaderDocumentHash("abcdef"));
}

TEST(KOReaderSyncProtocol, TreatsOfficialEmptyGetAsNotFound) {
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::GET_PROGRESS, 200, " { } \r\n"),
            KOReaderSyncResponse::NOT_FOUND);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::GET_PROGRESS, 200, R"({"progress":"/body/p[2]"})"),
            KOReaderSyncResponse::OK);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::GET_PROGRESS, 404, ""), KOReaderSyncResponse::NOT_FOUND);
}

TEST(KOReaderSyncProtocol, PreservesActionableAuthAndPayloadFailures) {
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::AUTHENTICATE, 401, R"({"code":2001})"),
            KOReaderSyncResponse::AUTH_FAILED);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::PUT_PROGRESS, 403, R"({"code":2003})"),
            KOReaderSyncResponse::INVALID_REQUEST);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::PUT_PROGRESS, 403, R"({"code":2004})"),
            KOReaderSyncResponse::INVALID_REQUEST);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::PUT_PROGRESS, 403, R"({"code":2001})"),
            KOReaderSyncResponse::ACCESS_DENIED);
  EXPECT_EQ(classifyKOReaderResponse(KOReaderSyncOperation::PUT_PROGRESS, 502, R"({"code":2000})"),
            KOReaderSyncResponse::SERVER_ERROR);
  EXPECT_EQ(parseKOReaderProtocolCode(R"({"code": 2003, "message":"Invalid request"})"), 2003);
}
