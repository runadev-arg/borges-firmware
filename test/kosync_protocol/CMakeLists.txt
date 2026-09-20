add_executable(KOReaderSyncProtocolTest
  KOReaderSyncProtocolTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncProtocol.cpp
)

target_include_directories(KOReaderSyncProtocolTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderSyncProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderSyncProtocolTest)
