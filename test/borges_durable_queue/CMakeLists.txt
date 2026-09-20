set(ARDUINOJSON_HOST_SOURCE "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson")
if(NOT EXISTS "${ARDUINOJSON_HOST_SOURCE}/src/ArduinoJson.h")
  FetchContent_Declare(arduinojson_host
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(arduinojson_host)
  if(NOT arduinojson_host_POPULATED)
    FetchContent_Populate(arduinojson_host)
  endif()
  set(ARDUINOJSON_HOST_SOURCE "${arduinojson_host_SOURCE_DIR}")
endif()

add_executable(BorgesDurableQueueTest BorgesDurableQueueTest.cpp BorgesReadingEventsTest.cpp ProgressFileTest.cpp BorgesProgressRecoveryTest.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesDurableQueue.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesReadingEvents.cpp
  ${REPO_ROOT}/lib/BorgesSync/BorgesSyncCore.cpp)
target_include_directories(BorgesDurableQueueTest PRIVATE
  stubs ${REPO_ROOT}/lib/BorgesSync ${ARDUINOJSON_HOST_SOURCE}/src ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src/activities/reader)
target_link_libraries(BorgesDurableQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BorgesDurableQueueTest)
