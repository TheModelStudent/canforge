canforge_add_test(sim_tests
  SOURCES test_sources.cpp test_vehicle.cpp test_ecu.cpp test_config.cpp
          test_faults.cpp
  LIBS canforge::sim canforge::dbc)
target_compile_definitions(sim_tests PRIVATE
  CANFORGE_SIM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
  CANFORGE_DBC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../../dbc/tests/data")
