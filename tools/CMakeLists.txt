# Standalone tools.
#   exchange_sim     - simulated exchange process. Linux-only (UDP).
#   latency_report   - in-process engine + exchange run, writes the report CSVs.
#
# exchange_sim depends on networking primitives that only exist on Linux, so it
# is only built there. latency_report is platform-independent.

function(hft_add_tool name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
endfunction()

hft_add_tool(exchange_sim)
hft_add_tool(latency_report)
hft_add_tool(itch50_replay)
