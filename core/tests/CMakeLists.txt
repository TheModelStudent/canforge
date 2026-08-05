canforge_add_test(core_tests
  SOURCES test_result.cpp test_frame.cpp test_signal.cpp
          test_signal_golden.cpp test_signal_property.cpp
  LIBS canforge::core)

# The allocation trap replaces the global operator new for the whole binary,
# so it gets a target of its own rather than contaminating the others.
canforge_add_test(core_no_alloc_test
  SOURCES test_no_alloc.cpp
  LIBS canforge::core)
# Replacing the global operator new/delete with malloc/free is exactly what
# this test is for, but GCC pairs the call sites and reports a mismatch.
target_compile_options(core_no_alloc_test PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
