canforge_add_test(transport_tests
  SOURCES test_timing.cpp test_virtual_bus.cpp test_logs.cpp
          test_inflate.cpp test_socketcan.cpp
  LIBS canforge::transport)
target_compile_definitions(transport_tests PRIVATE
  CANFORGE_TRANSPORT_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")

