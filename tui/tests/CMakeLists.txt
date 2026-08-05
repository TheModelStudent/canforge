canforge_add_test(tui_tests
  SOURCES test_viewmodel.cpp
  LIBS canforge::tui_model canforge::dbc)
target_compile_definitions(tui_tests PRIVATE
  CANFORGE_DBC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../../dbc/tests/data")
