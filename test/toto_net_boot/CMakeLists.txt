add_executable(TotoNetBootCodecTest
  TotoNetBootCodecTest.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoNetBootCodec.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoSyncCore.cpp
)

target_include_directories(TotoNetBootCodecTest PRIVATE
  ${REPO_ROOT}/lib/TotoSync
)

target_link_libraries(TotoNetBootCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TotoNetBootCodecTest)
