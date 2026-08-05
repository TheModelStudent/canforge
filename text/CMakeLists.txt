add_library(canforge_text
  src/Diagnostic.cpp
  src/Lexer.cpp)
add_library(canforge::text ALIAS canforge_text)

target_include_directories(canforge_text PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(canforge_text PUBLIC cxx_std_17)
# text sits directly above core: it needs Result and nothing else.
target_link_libraries(canforge_text PUBLIC canforge_core)
target_link_libraries(canforge_text PRIVATE canforge_warnings)
set_target_properties(canforge_text PROPERTIES EXPORT_NAME text)

install(TARGETS canforge_text EXPORT canforgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
