cmake_minimum_required(VERSION 3.21)

project(LinkObservatory
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral Fabric OS runtime for link health observation and historical interpretation"
  HOMEPAGE_URL "https://github.com/summonlabs/Link-Observatory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Single source of truth for on-disk and on-wire format versions.
# ---------------------------------------------------------------------------
set(LINKOBS_SNAPSHOT_FORMAT_VERSION 1)
set(LINKOBS_TRANSPORT_PROTOCOL_VERSION 1)
set(LINKOBS_LOR_FORMAT_VERSION 1)

option(LINKOBS_BUILD_APPS       "Build the linkobs command line runtime"     ON)
option(LINKOBS_BUILD_TESTS      "Build the Link Observatory test suites"    ON)
option(LINKOBS_BUILD_BENCHMARKS "Build the Link Observatory benchmarks"     ON)
option(LINKOBS_BUILD_EXAMPLES   "Build the Link Observatory examples"       ON)
option(LINKOBS_WARNINGS_AS_ERRORS "Treat first-party warnings as errors"    ON)
option(LINKOBS_ENABLE_ANALYZE   "Enable MSVC static analysis (/analyze)"    OFF)
option(LINKOBS_SANITIZE         "Sanitizer to enable (empty, address, undefined, thread)" "")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(LinkObservatoryWarnings)

# ---------------------------------------------------------------------------
# Generated public version header
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/linkobs/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# The runtime library
# ---------------------------------------------------------------------------
set(LINKOBS_SOURCES
  src/core/hash128.cpp
  src/core/sha256.cpp
  src/core/hex.cpp
  src/core/lock_tracker.cpp
  src/core/status.cpp
  src/core/text.cpp
  src/time/clock.cpp
  src/domain/enums.cpp
  src/domain/metric.cpp
  src/domain/observation.cpp
  src/domain/evidence.cpp
  src/domain/link.cpp
  src/domain/policy.cpp
  src/store/link_registry.cpp
  src/store/evidence_store.cpp
  src/store/history.cpp
  src/store/counter_tracker.cpp
  src/classify/classifier.cpp
  src/classify/explain.cpp
  src/persist/codec.cpp
  src/persist/snapshot.cpp
  src/ingest/record.cpp
  src/ingest/codec.cpp
  src/ingest/queue.cpp
  src/runtime/config.cpp
  src/runtime/worker.cpp
  src/runtime/observatory.cpp
  src/transport/socket.cpp
  src/transport/frame.cpp
  src/transport/server.cpp
  src/transport/client.cpp
)

add_library(linkobs ${LINKOBS_SOURCES})
add_library(LinkObservatory::linkobs ALIAS linkobs)

target_include_directories(linkobs
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(linkobs
  PUBLIC
    LINKOBS_FORMAT_VERSION_SNAPSHOT=${LINKOBS_SNAPSHOT_FORMAT_VERSION}
    LINKOBS_FORMAT_VERSION_TRANSPORT=${LINKOBS_TRANSPORT_PROTOCOL_VERSION}
    LINKOBS_FORMAT_VERSION_LOR=${LINKOBS_LOR_FORMAT_VERSION}
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:NOMINMAX>)

target_link_libraries(linkobs PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(linkobs PUBLIC ws2_32)
endif()

# Lock order / re-entrancy auditing. Enabled in Debug builds and whenever the
# dedicated option is on. It is a PUBLIC definition because LockScope and
# TrackedLock are header visible and must have one definition across the library
# and its consumers.
option(LINKOBS_LOCK_TRACKING "Enable runtime lock order and re-entrancy auditing" ON)
if(LINKOBS_LOCK_TRACKING)
  target_compile_definitions(linkobs PUBLIC LINKOBS_LOCK_TRACKING=1)
endif()

linkobs_apply_warnings(linkobs)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(LINKOBS_BUILD_APPS)
  add_executable(linkobs_cli
    apps/linkobs/main.cpp
    apps/linkobs/cmd_ingest.cpp
    apps/linkobs/cmd_report.cpp
    apps/linkobs/cmd_transport.cpp
    apps/linkobs/cmd_tools.cpp)
  set_target_properties(linkobs_cli PROPERTIES OUTPUT_NAME linkobs)
  target_include_directories(linkobs_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/linkobs")
  target_link_libraries(linkobs_cli PRIVATE LinkObservatory::linkobs)
  linkobs_apply_warnings(linkobs_cli)
endif()

# ---------------------------------------------------------------------------
# Tests, benchmarks, examples
# ---------------------------------------------------------------------------
if(LINKOBS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LINKOBS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(LINKOBS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS linkobs
  EXPORT LinkObservatoryTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LINKOBS_BUILD_APPS)
  install(TARGETS linkobs_cli
    EXPORT LinkObservatoryTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY include/linkobs DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/linkobs/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/linkobs")

set(LINKOBS_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/LinkObservatory")

install(EXPORT LinkObservatoryTargets
  FILE LinkObservatoryTargets.cmake
  NAMESPACE LinkObservatory::
  DESTINATION "${LINKOBS_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LinkObservatoryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LinkObservatoryConfig.cmake"
  INSTALL_DESTINATION "${LINKOBS_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LinkObservatoryConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LinkObservatoryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LinkObservatoryConfigVersion.cmake"
  DESTINATION "${LINKOBS_INSTALL_CMAKEDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/LinkObservatory")

message(STATUS "Link Observatory ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | tests: ${LINKOBS_BUILD_TESTS} | sanitize: '${LINKOBS_SANITIZE}'")
