add_library(canforge_core
  src/Result.cpp
  src/Frame.cpp
  src/Signal.cpp
  src/Message.cpp
  src/Database.cpp)
add_library(canforge::core ALIAS canforge_core)

target_include_directories(canforge_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(canforge_core PUBLIC cxx_std_17)
target_link_libraries(canforge_core PRIVATE canforge_warnings)
set_target_properties(canforge_core PROPERTIES EXPORT_NAME core)

install(TARGETS canforge_core EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
