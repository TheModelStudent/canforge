cmake_minimum_required(VERSION 3.20)
project(canforge VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "A CAN bus toolkit in modern C++17")

# PROJECT_IS_TOP_LEVEL only exists from 3.21; we support 3.20.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
  if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PROJECT_IS_TOP_LEVEL ON)
  else()
    set(PROJECT_IS_TOP_LEVEL OFF)
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(CANFORGE_WERROR      "Treat warnings as errors"        ON)
option(CANFORGE_BUILD_TESTS "Build the test suite"            ${PROJECT_IS_TOP_LEVEL})
option(CANFORGE_BUILD_TUI   "Build the ftxui dashboard"       ${PROJECT_IS_TOP_LEVEL})
option(CANFORGE_BUILD_CLI   "Build the command line tools"    ${PROJECT_IS_TOP_LEVEL})
option(CANFORGE_BUILD_BENCH "Build the micro benchmarks"      ${PROJECT_IS_TOP_LEVEL})
option(CANFORGE_BUILD_FUZZ  "Build the fuzz targets"          ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Warning flags live in one INTERFACE target that every canforge target links
# PRIVATE. Because a PRIVATE link on a static library still shows up in the
# export set as $<LINK_ONLY:...>, canforge_warnings has to be installed and
# exported too -- it is an internal detail of the package and carries no
# usage requirements for consumers.
add_library(canforge_warnings INTERFACE)
add_library(canforge::warnings ALIAS canforge_warnings)
target_compile_options(canforge_warnings INTERFACE
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wold-style-cast -Wnon-virtual-dtor -Wdouble-promotion -Wformat=2
    -Wcast-qual -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
    $<$<BOOL:${CANFORGE_WERROR}>:-Werror>>
  $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:
    /W4 /permissive- $<$<BOOL:${CANFORGE_WERROR}>:/WX>>)

if(CANFORGE_BUILD_TESTS)
  enable_testing()
  include(cmake/GoogleTest.cmake)
endif()

add_subdirectory(core)
add_subdirectory(text)
add_subdirectory(dbc)
add_subdirectory(transport)
add_subdirectory(isotp)
add_subdirectory(uds)
add_subdirectory(sim)
add_subdirectory(tui)
if(CANFORGE_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(CANFORGE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(CANFORGE_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

# Install / export
install(TARGETS canforge_warnings EXPORT canforgeTargets)
install(EXPORT canforgeTargets
        FILE canforgeTargets.cmake
        NAMESPACE canforge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/canforge)

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/canforge-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/canforge-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/canforge)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/canforge-config-version.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/canforge-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/canforge-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/canforge)
