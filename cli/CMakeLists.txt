add_executable(canforge src/canforge.cpp src/Flash.cpp)
target_link_libraries(canforge PRIVATE
  canforge::core canforge::dbc canforge::codegen canforge::transport
  canforge::isotp canforge::uds canforge::sim canforge::text)
if(CANFORGE_BUILD_TUI)
  target_link_libraries(canforge PRIVATE canforge::tui)
  target_compile_definitions(canforge PRIVATE CANFORGE_HAVE_TUI=1)
else()
  target_link_libraries(canforge PRIVATE canforge::tui_model)
endif()
target_link_libraries(canforge PRIVATE canforge_warnings)
install(TARGETS canforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
