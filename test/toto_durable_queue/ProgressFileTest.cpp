#include <gtest/gtest.h>

#include "ProgressFile.h"

class ReaderProgressPersistence : public ::testing::Test {
 protected:
  void SetUp() override { Storage = {}; }
  void TearDown() override { Storage = {}; }
  static std::vector<uint8_t> read() {
    HalFile file;
    if (!ProgressFile::openForRead("TEST", "/book", file)) return {};
    std::vector<uint8_t> data(file.fileSize());
    file.read(data.data(), data.size());
    return data;
  }
};

TEST_F(ReaderProgressPersistence, InterruptedReplacementStillOpensLastCompletePosition) {
  Storage.files["/book/progress.bin"] = {1, 2, 3, 4, 5, 6};
  const uint8_t next[] = {9, 8, 7, 6, 5, 4};
  // Saving the backup succeeds; publication and rollback both fail, as if SD
  // disappeared or power was lost before the new canonical file was published.
  Storage.failRenameFromCall = 2;
  EXPECT_FALSE(ProgressFile::writeAtomic("/book", next, sizeof(next)));
  EXPECT_FALSE(Storage.exists("/book/progress.bin"));
  EXPECT_EQ(read(), (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
  Storage.failRenameFromCall = 0;
  ASSERT_TRUE(ProgressFile::writeAtomic("/book", next, sizeof(next)));
  EXPECT_EQ(read(), (std::vector<uint8_t>{9, 8, 7, 6, 5, 4}));
  EXPECT_EQ(Storage.files.at("/book/progress.bin.bak"), (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST_F(ReaderProgressPersistence, FailedBackupRenameDoesNotRemoveCanonicalPosition) {
  Storage.files["/book/progress.bin"] = {1, 2, 3, 4};
  const uint8_t next[] = {9, 8, 7, 6};
  Storage.failRename = true;
  EXPECT_FALSE(ProgressFile::writeAtomic("/book", next, sizeof(next)));
  EXPECT_EQ(read(), (std::vector<uint8_t>{1, 2, 3, 4}));
}
