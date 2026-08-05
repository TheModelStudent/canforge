# The view model has no ftxui in it, so it builds and is tested everywhere.
add_library(canforge_tui_model src/ViewModel.cpp)
add_library(canforge::tui_model ALIAS canforge_tui_model)
target_include_directories(canforge_tui_model PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(canforge_tui_model PUBLIC canforge_core canforge_transport)
target_link_libraries(canforge_tui_model PRIVATE canforge_warnings)
set_target_properties(canforge_tui_model PROPERTIES EXPORT_NAME tui_model)
install(TARGETS canforge_tui_model EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# The dashboard itself is the only thing that needs ftxui, so it is the only
# thing behind CANFORGE_BUILD_TUI. The view model and its tests build even
# when the option is off, which is what lets the sanitizer and fuzzing builds
# skip the dependency entirely.
if(NOT CANFORGE_BUILD_TUI)
  return()
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Ftxui.cmake)
add_library(canforge_tui src/Dashboard.cpp)
add_library(canforge::tui ALIAS canforge_tui)
target_include_directories(canforge_tui PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(canforge_tui PUBLIC canforge_tui_model)
target_link_libraries(canforge_tui PRIVATE
  ftxui::screen ftxui::dom ftxui::component canforge_warnings)


