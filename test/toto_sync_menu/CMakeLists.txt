add_executable(TotoSyncMenuTest
  TotoSyncMenuTest.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoSyncMenu.cpp
)

target_include_directories(TotoSyncMenuTest PRIVATE
  ${REPO_ROOT}/lib/TotoSync
)

target_link_libraries(TotoSyncMenuTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TotoSyncMenuTest)
