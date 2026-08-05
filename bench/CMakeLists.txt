add_executable(canforge_bench_codec bench_codec.cpp)
target_link_libraries(canforge_bench_codec PRIVATE canforge::core canforge_warnings)

add_executable(canforge_bench_pipeline bench_pipeline.cpp)
target_link_libraries(canforge_bench_pipeline PRIVATE
  canforge::dbc canforge::transport canforge_warnings)

add_executable(canforge_soak soak.cpp)
target_link_libraries(canforge_soak PRIVATE
  canforge::dbc canforge::sim canforge::tui_model canforge_warnings)
