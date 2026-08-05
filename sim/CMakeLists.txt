add_library(canforge_sim
  src/SignalSource.cpp
  src/Vehicle.cpp
  src/Ecu.cpp
  src/Faults.cpp
  src/Config.cpp
  src/Simulator.cpp
  src/UdsServer.cpp)
add_library(canforge::sim ALIAS canforge_sim)

target_include_directories(canforge_sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(canforge_sim PUBLIC
  canforge_core canforge_text canforge_transport canforge_isotp canforge_uds)
target_link_libraries(canforge_sim PRIVATE canforge_warnings)
set_target_properties(canforge_sim PROPERTIES EXPORT_NAME sim)

install(TARGETS canforge_sim EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
