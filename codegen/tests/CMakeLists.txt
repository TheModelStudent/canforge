canforge_add_test(codegen_tests
  SOURCES test_cgen.cpp
  LIBS canforge::codegen canforge::dbc)
target_compile_definitions(codegen_tests PRIVATE
  CANFORGE_CODEGEN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
  CANFORGE_DBC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../../dbc/tests/data")
