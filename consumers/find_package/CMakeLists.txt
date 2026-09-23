# Independent downstream consumer of the installed Fabric Observatory package.
#
# This project deliberately does not add_subdirectory the runtime, does not use
# FetchContent and does not reference any path inside the source tree. It is
# configured on its own against an installed prefix.

cmake_minimum_required(VERSION 3.25)

project(FabricObservatoryDownstream CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FabricObservatory 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE FabricObservatory::fabric_observatory)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
endif()

# Report what the package actually provided, so the proof is visible in the log.
get_target_property(_fabobs_include FabricObservatory::fabric_observatory INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "downstream: FabricObservatory_DIR=${FabricObservatory_DIR}")
message(STATUS "downstream: include directories=${_fabobs_include}")
