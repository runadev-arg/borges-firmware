add_executable(TotoSyncCoreTest
  TotoSyncCoreTest.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoSyncCore.cpp
)

target_include_directories(TotoSyncCoreTest PRIVATE
  ${REPO_ROOT}/lib/TotoSync
)

target_link_libraries(TotoSyncCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TotoSyncCoreTest)
