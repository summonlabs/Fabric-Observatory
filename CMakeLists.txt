cmake_minimum_required(VERSION 3.25)

project(FabricObservatory
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral Fabric OS observational runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Observatory"
  LANGUAGES CXX)

set(FABRIC_OBSERVATORY_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(FABRIC_OBSERVATORY_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(FABRIC_OBSERVATORY_VERSION_PATCH ${PROJECT_VERSION_PATCH})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FabricObservatoryWarnings)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FABRIC_OBSERVATORY_IS_TOP_LEVEL ON)
else()
  set(FABRIC_OBSERVATORY_IS_TOP_LEVEL OFF)
endif()

option(FABRIC_OBSERVATORY_BUILD_TESTS "Build the Fabric Observatory test suite" ${FABRIC_OBSERVATORY_IS_TOP_LEVEL})
option(FABRIC_OBSERVATORY_BUILD_TOOLS "Build the fabobs command line tool" ON)
option(FABRIC_OBSERVATORY_BUILD_EXAMPLES "Build Fabric Observatory examples" ${FABRIC_OBSERVATORY_IS_TOP_LEVEL})
option(FABRIC_OBSERVATORY_BUILD_BENCHMARKS "Build Fabric Observatory benchmarks" ${FABRIC_OBSERVATORY_IS_TOP_LEVEL})
option(FABRIC_OBSERVATORY_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FABRIC_OBSERVATORY_ENABLE_ASAN "Build with AddressSanitizer when the toolchain supports it" OFF)
option(FABRIC_OBSERVATORY_LOCK_AUDIT "Enable the runtime lock ordering and reentrancy auditor" OFF)
option(FABRIC_OBSERVATORY_DOWNSTREAM_PROOF
  "Install into a staging prefix and prove the package from an independent find_package consumer" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(FABRIC_OBSERVATORY_LOCK_AUDIT)
  add_compile_definitions(FABRIC_OBSERVATORY_LOCK_AUDIT=1)
endif()

add_library(fabric_observatory STATIC)
add_library(FabricObservatory::fabric_observatory ALIAS fabric_observatory)

target_sources(fabric_observatory PRIVATE
  src/status.cpp
  src/util.cpp
  src/digest.cpp
  src/time.cpp
  src/ids.cpp
  src/canonical.cpp
  src/value.cpp
  src/json.cpp
  src/truth.cpp
  src/aspect.cpp
  src/observation.cpp
  src/policy.cpp
  src/snapshot.cpp
  src/query.cpp
  src/diff.cpp
  src/history.cpp
  src/explanation.cpp
  src/persistence.cpp
  src/ingest.cpp
  src/observatory.cpp
  src/transport.cpp
  src/lock_audit.cpp
  src/json_io.cpp)

target_include_directories(fabric_observatory
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(fabric_observatory PUBLIC cxx_std_20)
fabric_observatory_apply_warnings(fabric_observatory)

find_package(Threads REQUIRED)
target_link_libraries(fabric_observatory PUBLIC Threads::Threads)

if(FABRIC_OBSERVATORY_ENABLE_ASAN)
  fabric_observatory_enable_asan(fabric_observatory)
endif()

# ---------------------------------------------------------------- tools
if(FABRIC_OBSERVATORY_BUILD_TOOLS)
  add_executable(fabobs tools/fabobs_main.cpp)
  target_link_libraries(fabobs PRIVATE FabricObservatory::fabric_observatory)
  fabric_observatory_apply_warnings(fabobs)
  if(FABRIC_OBSERVATORY_ENABLE_ASAN)
    fabric_observatory_enable_asan(fabobs)
  endif()
endif()

# ------------------------------------------------------------- examples
if(FABRIC_OBSERVATORY_BUILD_EXAMPLES)
  foreach(example ingest_walkthrough embedded_query ndjson_bridge)
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE FabricObservatory::fabric_observatory)
    fabric_observatory_apply_warnings(example_${example})
    if(FABRIC_OBSERVATORY_ENABLE_ASAN)
      fabric_observatory_enable_asan(example_${example})
    endif()
  endforeach()
endif()

# ----------------------------------------------------------- benchmarks
if(FABRIC_OBSERVATORY_BUILD_BENCHMARKS)
  foreach(bench bench_ingest bench_snapshot bench_query bench_persistence)
    add_executable(${bench} benchmarks/${bench}.cpp)
    target_link_libraries(${bench} PRIVATE FabricObservatory::fabric_observatory)
    fabric_observatory_apply_warnings(${bench})
    if(FABRIC_OBSERVATORY_ENABLE_ASAN)
      fabric_observatory_enable_asan(${bench})
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------- tests
if(FABRIC_OBSERVATORY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -------------------------------------------------------------- install
install(TARGETS fabric_observatory
  EXPORT FabricObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FABRIC_OBSERVATORY_BUILD_TOOLS)
  install(TARGETS fabobs
    EXPORT FabricObservatoryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT FabricObservatoryTargets
  FILE FabricObservatoryTargets.cmake
  NAMESPACE FabricObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricObservatory)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricObservatoryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricObservatoryConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricObservatory)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FabricObservatoryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FabricObservatoryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricObservatoryConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricObservatory)

export(EXPORT FabricObservatoryTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/FabricObservatoryTargets.cmake"
  NAMESPACE FabricObservatory::)

# ------------------------------------------- downstream packaging proof
# Installs the runtime into a staging prefix and builds the consumer project in
# consumers/find_package against that prefix alone. Registered as a test so the
# proof runs with the rest of the suite.
if(FABRIC_OBSERVATORY_DOWNSTREAM_PROOF AND FABRIC_OBSERVATORY_BUILD_TESTS)
  set(FABRIC_OBSERVATORY_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/downstream-prefix")
  set(FABRIC_OBSERVATORY_CONSUMER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/consumers/find_package")
  set(FABRIC_OBSERVATORY_CONSUMER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/downstream-build")
  # A sanitized static library can only be linked by a sanitized consumer, so the
  # consumer inherits the sanitizer flags and the sanitizer runtime location.
  if(FABRIC_OBSERVATORY_ENABLE_ASAN AND MSVC)
    set(FABRIC_OBSERVATORY_DOWNSTREAM_COMPILE_FLAGS "/fsanitize=address")
    set(FABRIC_OBSERVATORY_DOWNSTREAM_LINK_FLAGS "/fsanitize=address")
    get_filename_component(FABRIC_OBSERVATORY_ASAN_DLL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  else()
    set(FABRIC_OBSERVATORY_DOWNSTREAM_COMPILE_FLAGS "")
    set(FABRIC_OBSERVATORY_DOWNSTREAM_LINK_FLAGS "")
    set(FABRIC_OBSERVATORY_ASAN_DLL_DIR "")
  endif()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownstreamConsumerTest.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DownstreamConsumerTest.cmake"
    @ONLY)
  add_test(NAME downstream_find_package
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/DownstreamConsumerTest.cmake")
  set_tests_properties(downstream_find_package PROPERTIES TIMEOUT 0)
endif()
