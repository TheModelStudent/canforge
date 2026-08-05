add_library(canforge_transport
  src/IBus.cpp
  src/VirtualBus.cpp
  src/SocketCanBus.cpp
  src/LogFormat.cpp
  src/BlfLog.cpp
  src/Inflate.cpp
  src/LogReplayBus.cpp)
add_library(canforge::transport ALIAS canforge_transport)

target_include_directories(canforge_transport PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# transport sits above core only; it knows nothing about dbc.
target_link_libraries(canforge_transport PUBLIC canforge_core)
target_link_libraries(canforge_transport PRIVATE canforge_warnings)
find_package(Threads REQUIRED)
target_link_libraries(canforge_transport PUBLIC Threads::Threads)
set_target_properties(canforge_transport PROPERTIES EXPORT_NAME transport)

install(TARGETS canforge_transport EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
