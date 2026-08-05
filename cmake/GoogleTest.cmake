# GoogleTest is fetched, not vendored, and is pulled in only when
# CANFORGE_BUILD_TESTS is on, so the core library never depends on it.
include(FetchContent)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2   # pinned; bump deliberately
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Fetched dependencies must never inherit the project warning flags, and their
# headers are marked SYSTEM so -Werror cannot be tripped by them.
foreach(_t gtest gtest_main gmock gmock_main)
  if(TARGET ${_t})
    get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()

# canforge_add_test(<name> SOURCES ... [LIBS ...])
function(canforge_add_test name)
  cmake_parse_arguments(A "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE ${A_LIBS} GTest::gtest_main)
  target_link_libraries(${name} PRIVATE canforge_warnings)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()
