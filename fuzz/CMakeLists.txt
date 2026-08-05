# Fuzz targets are written against the libFuzzer entry point so that
#   clang++ -fsanitize=fuzzer,address fuzz_dbc.cpp
# works unchanged. CANFORGE_FUZZ_LIBFUZZER=ON builds them that way; the default
# links standalone_main.cpp instead, which needs no clang and lets the same
# targets run under GCC with ASan.
option(CANFORGE_FUZZ_LIBFUZZER "Build the fuzz targets against libFuzzer" OFF)

set(_fuzz_targets fuzz_dbc fuzz_blf fuzz_asc fuzz_isotp)
set(_fuzz_libs_fuzz_dbc   canforge::dbc)
set(_fuzz_libs_fuzz_blf   canforge::transport)
set(_fuzz_libs_fuzz_asc   canforge::transport)
set(_fuzz_libs_fuzz_isotp canforge::isotp)

foreach(_target ${_fuzz_targets})
  if(CANFORGE_FUZZ_LIBFUZZER)
    add_executable(${_target} ${_target}.cpp)
    target_compile_options(${_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${_target} PRIVATE -fsanitize=fuzzer,address,undefined)
  else()
    add_executable(${_target} ${_target}.cpp standalone_main.cpp)
  endif()
  target_link_libraries(${_target} PRIVATE ${_fuzz_libs_${_target}} canforge_warnings)
endforeach()

# Replaying the committed corpus on every build is what turns a crash found
# once into a regression test that stays found.
if(CANFORGE_BUILD_TESTS AND NOT CANFORGE_FUZZ_LIBFUZZER)
  foreach(_target ${_fuzz_targets})
    string(REPLACE "fuzz_" "" _corpus ${_target})
    add_test(NAME fuzz_corpus_${_corpus}
             COMMAND ${_target} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_corpus})
  endforeach()
endif()
