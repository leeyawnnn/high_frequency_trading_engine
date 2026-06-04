# Unit tests. These are explicitly NOT hot-path code, so they are allowed to
# allocate, use the STL freely, and are the targets we run under sanitizers.

# Helper: register a single-file test executable + CTest entry.
function(hft_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

hft_add_test(test_toolchain)
hft_add_test(test_tsc)
hft_add_test(test_histogram)
hft_add_test(test_spsc)
hft_add_test(test_itch)
hft_add_test(test_net)
hft_add_test(test_config)
hft_add_test(test_feed)
hft_add_test(test_book)
hft_add_test(test_strategy)
hft_add_test(test_risk)
hft_add_test(test_gateway)
hft_add_test(test_engine)
