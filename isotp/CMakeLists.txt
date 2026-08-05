add_library(canforge_isotp src/IsoTp.cpp)
add_library(canforge::isotp ALIAS canforge_isotp)
target_include_directories(canforge_isotp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# isotp needs only frames and Result: it never touches a bus.
target_link_libraries(canforge_isotp PUBLIC canforge_core)
target_link_libraries(canforge_isotp PRIVATE canforge_warnings)
set_target_properties(canforge_isotp PROPERTIES EXPORT_NAME isotp)
install(TARGETS canforge_isotp EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
