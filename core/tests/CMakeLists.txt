canforge_add_test(core_tests
  SOURCES test_result.cpp test_frame.cpp test_signal.cpp
          test_signal_golden.cpp test_signal_property.cpp
  LIBS canforge::core)

# The allocation trap replaces the global operator new for the whole binary, so
# it gets a target of its own rather than contaminating the others.
#
# It also cannot coexist with a sanitizer: TSan's runtime defines operator new
# itself, so the two collide at link time and the build fails before any test
# runs. Sanitizer builds therefore turn this target off entirely; the proof runs
# unsanitised in the ordinary build jobs.
option(CANFORGE_BUILD_NO_ALLOC_TEST
       "Build the allocation-trap proof (conflicts with sanitizer runtimes)" ON)

if(CANFORGE_BUILD_NO_ALLOC_TEST)
  canforge_add_test(core_no_alloc_test
    SOURCES test_no_alloc.cpp
    LIBS canforge::core)
  # Replacing the global operator new/delete with malloc/free is exactly what
  # this test is for, but GCC pairs the call sites and reports a mismatch.
  target_compile_options(core_no_alloc_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
endif()
