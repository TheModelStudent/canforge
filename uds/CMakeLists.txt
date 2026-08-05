add_library(canforge_uds src/Uds.cpp)
add_library(canforge::uds ALIAS canforge_uds)
target_include_directories(canforge_uds PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# uds sits directly on isotp and nothing else.
target_link_libraries(canforge_uds PUBLIC canforge_isotp)
target_link_libraries(canforge_uds PRIVATE canforge_warnings)
set_target_properties(canforge_uds PROPERTIES EXPORT_NAME uds)
install(TARGETS canforge_uds EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
