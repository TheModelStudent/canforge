add_library(canforge_dbc
  src/Parser.cpp
  src/Writer.cpp)
add_library(canforge::dbc ALIAS canforge_dbc)

target_include_directories(canforge_dbc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# dbc sits directly above core and knows about nothing else.
target_link_libraries(canforge_dbc PUBLIC canforge_core canforge_text)
target_link_libraries(canforge_dbc PRIVATE canforge_warnings)
set_target_properties(canforge_dbc PROPERTIES EXPORT_NAME dbc)

install(TARGETS canforge_dbc EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
