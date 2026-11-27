add_library(canforge_codegen
  src/CGen.cpp)
add_library(canforge::codegen ALIAS canforge_codegen)

target_include_directories(canforge_codegen PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# codegen reads a Database and writes text. It needs the model and the codec's
# layout arithmetic from core and nothing else: not the parser, not a bus.
target_link_libraries(canforge_codegen PUBLIC canforge_core)
target_link_libraries(canforge_codegen PRIVATE canforge_warnings)
set_target_properties(canforge_codegen PROPERTIES EXPORT_NAME codegen)

install(TARGETS canforge_codegen EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CANFORGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
