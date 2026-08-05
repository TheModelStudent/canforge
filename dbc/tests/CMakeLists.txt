canforge_add_test(dbc_tests
  SOURCES test_lexer.cpp test_parser.cpp test_roundtrip.cpp test_errors.cpp
  LIBS canforge::dbc)
target_compile_definitions(dbc_tests PRIVATE
  CANFORGE_DBC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
