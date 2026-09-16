add_executable(TotoDeviceLoginTest
  TotoDeviceLoginTest.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoDeviceLogin.cpp
  ${REPO_ROOT}/lib/TotoSync/TotoSyncCore.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(TotoDeviceLoginTest PRIVATE
  ${REPO_ROOT}/lib/TotoSync
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(TotoDeviceLoginTest PRIVATE
  TOTO_LOGIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(TotoDeviceLoginTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TotoDeviceLoginTest)
