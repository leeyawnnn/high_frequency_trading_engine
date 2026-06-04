# Standalone tools.
#   exchange_sim     - simulated exchange process (Phase 4). Linux-only (UDP).
#   latency_report   - offline histogram aggregation / pretty-printer (Phase 1+).
#
# exchange_sim depends on networking primitives that only exist on Linux, so it
# is only built there. latency_report is platform-independent.

function(hft_add_tool name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
endfunction()

hft_add_tool(exchange_sim)
hft_add_tool(latency_report)
