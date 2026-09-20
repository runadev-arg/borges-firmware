add_executable(BorgesNetBootCodecTest
  BorgesNetBootCodecTest.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesNetBootCodec.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesSyncCore.cpp
)

target_include_directories(BorgesNetBootCodecTest PRIVATE
  ${REPO_ROOT}/lib/BorgesSync
)

target_link_libraries(BorgesNetBootCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BorgesNetBootCodecTest)
