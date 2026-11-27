add_library(canforge_uds src/Uds.cpp src/BusLink.cpp src/DidFile.cpp)
add_library(canforge::uds ALIAS canforge_uds)
target_include_directories(canforge_uds PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# uds sits on isotp for the protocol and on transport for BusLink, which runs the
# client on a bus. Neither knows about dbc.
target_link_libraries(canforge_uds PUBLIC canforge_isotp canforge_transport)
# DID files are JSON; the reader lives in text.
target_link_libraries(canforge_uds PRIVATE canforge_text)
target_link_libraries(canforge_uds PRIVATE canforge_warnings)
set_target_properties(canforge_uds PROPERTIES EXPORT_NAME uds)
install(TARGETS canforge_uds EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
