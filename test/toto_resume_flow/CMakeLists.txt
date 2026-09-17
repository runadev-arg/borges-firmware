add_executable(TotoResumeFlowTest
  TotoResumeFlowTest.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoResumeFlow.cpp
)

target_include_directories(TotoResumeFlowTest PRIVATE
  ${REPO_ROOT}/lib/TotoSync
)

target_link_libraries(TotoResumeFlowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TotoResumeFlowTest)
