canforge_add_test(uds_tests
  SOURCES test_uds.cpp test_bus_link.cpp test_did_file.cpp
  LIBS canforge::uds canforge::sim)
