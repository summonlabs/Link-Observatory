# Link Observatory test suites.
#
# Every suite is a plain executable with a deterministic exit code. No test uses
# a timeout: concurrency is driven by explicit handshakes and time dependent
# behaviour is driven by ManualClock.

add_library(linkobs_test_support STATIC
  support/harness.cpp
  support/fixtures.cpp)
target_include_directories(linkobs_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(linkobs_test_support PUBLIC LinkObservatory::linkobs)
linkobs_apply_warnings(linkobs_test_support)

set(LINKOBS_TEST_SOURCES
  test_core.cpp
  test_domain.cpp
  test_record.cpp
  test_store.cpp
  test_classify.cpp
  test_persist.cpp
  test_property.cpp
  test_adversarial.cpp
  test_hardening.cpp
  test_concurrency.cpp
  test_integration.cpp
  test_transport.cpp)

add_executable(linkobs_tests ${LINKOBS_TEST_SOURCES})
target_link_libraries(linkobs_tests PRIVATE linkobs_test_support)
linkobs_apply_warnings(linkobs_tests)

if(WIN32)
  target_link_libraries(linkobs_tests PRIVATE ws2_32)
endif()

add_test(NAME linkobs.unit COMMAND linkobs_tests)

if(TARGET linkobs_cli)
  add_executable(linkobs_process_tests process_tests.cpp)
  target_link_libraries(linkobs_process_tests PRIVATE linkobs_test_support)
  target_compile_definitions(linkobs_process_tests PRIVATE
    LINKOBS_CLI_PATH="$<TARGET_FILE:linkobs_cli>")
  linkobs_apply_warnings(linkobs_process_tests)
  add_test(NAME linkobs.process COMMAND linkobs_process_tests)
endif()
