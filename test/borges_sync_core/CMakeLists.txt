add_executable(BorgesSyncCoreTest
  BorgesSyncCoreTest.cpp
  BorgesMigrationTest.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesSyncCore.cpp
)

target_include_directories(BorgesSyncCoreTest PRIVATE
  ${REPO_ROOT}/lib/BorgesSync
)

target_link_libraries(BorgesSyncCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BorgesSyncCoreTest)
