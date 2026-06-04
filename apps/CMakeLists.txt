# Top-level application: the full engine wiring feed -> strategy -> gateway,
# with thread pinning driven by a JSON config (Phase 5+). Linux-only.
#
function(hft_add_app name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
endfunction()

hft_add_app(hft_main)
