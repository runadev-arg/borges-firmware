#include <gtest/gtest.h>
#include <set>
#include <string>
#include "../../src/BorgesMigration.h"

struct MigrationStorage {
  std::set<std::string> paths;
  std::string failAt;
  bool exists(const char* path) const { return paths.count(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (failAt == from) return false;
    std::set<std::string> next;
    for (const auto& path : paths) {
      if (path == from || path.rfind(std::string(from) + "/", 0) == 0)
        next.insert(std::string(to) + path.substr(std::string(from).size()));
      else next.insert(path);
    }
    paths = next;
    return true;
  }
};

TEST(BorgesMigration, PreservesCredentialsAndQueuedEventsAndIsIdempotent) {
  MigrationStorage storage{{"/.crosspoint", "/.crosspoint/settings.json", "/.crosspoint/toto.json",
                            "/.crosspoint/toto", "/.crosspoint/toto/outbox/1.json"}, ""};
  ASSERT_TRUE(migrateBorgesStorage(storage));
  EXPECT_TRUE(storage.exists("/.borges/settings.json"));
  EXPECT_TRUE(storage.exists("/.borges/borges.json"));
  EXPECT_TRUE(storage.exists("/.borges/borges/outbox/1.json"));
  auto paths = storage.paths;
  EXPECT_TRUE(migrateBorgesStorage(storage));
  EXPECT_EQ(paths, storage.paths);
}

TEST(BorgesMigration, InterruptedMigrationCanResumeWithoutLosingData) {
  MigrationStorage storage{{"/.crosspoint", "/.crosspoint/toto.json", "/.crosspoint/toto"}, "/.borges/toto.json"};
  EXPECT_FALSE(migrateBorgesStorage(storage));
  EXPECT_TRUE(storage.exists("/.borges/toto.json"));
  storage.failAt.clear();
  ASSERT_TRUE(migrateBorgesStorage(storage));
  EXPECT_TRUE(storage.exists("/.borges/borges.json"));
}

TEST(BorgesMigration, NeverOverwritesAnExistingBorgesInstallation) {
  MigrationStorage storage{{"/.borges", "/.borges/settings.json", "/.crosspoint", "/.crosspoint/settings.json"}, ""};
  const auto before = storage.paths;
  EXPECT_TRUE(migrateBorgesStorage(storage));
  EXPECT_EQ(before, storage.paths);
}
