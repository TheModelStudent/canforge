# ftxui is fetched, not vendored, and only when the dashboard is being built.
# Nothing below tui/ links it, so the library layers stay dependency free.
include(FetchContent)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v5.0.0   # pinned; bump deliberately
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(ftxui)

# Fetched code must not inherit the project's warning flags, and its headers
# are marked SYSTEM so -Werror cannot be tripped by them. Only the real
# targets are touched: set_target_properties refuses to work on an ALIAS.
foreach(_t screen dom component)
  if(TARGET ${_t})
    get_target_property(_aliased ${_t} ALIASED_TARGET)
    if(NOT _aliased)
      get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
      if(_inc)
        set_target_properties(${_t} PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
      endif()
    endif()
  endif()
endforeach()
