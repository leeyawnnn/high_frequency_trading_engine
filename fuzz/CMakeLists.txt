# libFuzzer targets. Off by default and Clang-only: -fsanitize=fuzzer is a
# Clang feature, and the zero-dependency default build must not require it.
#
# The sanitizers are combined with the fuzzer deliberately. A fuzzer without
# ASan finds far less, because much of what it produces is a memory error that
# happens not to crash on its own.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "HFT_BUILD_FUZZERS=ON requires Clang; skipping fuzz targets.")
  return()
endif()

# Apple's Command Line Tools clang accepts -fsanitize=fuzzer but ships no
# libclang_rt.fuzzer_osx.a, so the flag compiles and then fails at link. Probe
# by actually linking rather than by inspecting the compiler id, and say so
# clearly instead of failing the build with a missing-library error.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_cxx_source_compiles([[
  #include <cstddef>
  #include <cstdint>
  extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
]] HFT_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(NOT HFT_HAVE_LIBFUZZER)
  message(WARNING
    "HFT_BUILD_FUZZERS=ON but this toolchain cannot link libFuzzer "
    "(Apple Command Line Tools clang does not ship the fuzzer runtime). "
    "Use a full LLVM clang, or Linux. Skipping fuzz targets.")
  return()
endif()

function(hft_add_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE hft)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

hft_add_fuzzer(fuzz_book)
hft_add_fuzzer(fuzz_itch)
