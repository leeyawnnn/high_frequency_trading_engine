# Microbenchmarks. Built with full optimization (they link hft -> hft_flags).
# Each phase that has a perf target adds its bench_*.cpp here.
#
# Helper mirrors hft_add_test but without CTest registration (benchmarks are
# run manually and their numbers recorded in the README).
function(hft_add_benchmark name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
endfunction()

hft_add_benchmark(bench_spsc)
hft_add_benchmark(bench_feed)
hft_add_benchmark(bench_book)
hft_add_benchmark(bench_risk)
