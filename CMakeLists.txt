cmake_minimum_required(VERSION 3.20)

project(hft_engine
  VERSION 0.1.0
  DESCRIPTION "Sub-microsecond HFT-style trading engine (feed -> strategy -> gateway)"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Release-style build: this project only makes sense optimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "No build type specified; defaulting to Release.")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HFT_NATIVE     "Compile with -march=native (required for honest latency numbers)" ON)
option(HFT_LTO        "Enable link-time optimization (-flto)"                              ON)
option(HFT_WERROR     "Treat warnings as errors"                                           ON)
option(HFT_SANITIZE   "Build with ASan+UBSan (non-hot-path tests only; disables LTO)"      OFF)
option(HFT_TSAN       "Build with ThreadSanitizer (queue/concurrency tests; disables LTO)" OFF)
option(HFT_BUILD_TESTS      "Build unit tests"   ON)
option(HFT_BUILD_BENCHMARKS "Build benchmarks"   ON)
option(HFT_BUILD_TOOLS      "Build tools (exchange_sim, latency_report)" ON)
option(HFT_BUILD_APPS       "Build apps (hft_main)" ON)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
# The full engine (kernel-bypass-style loopback UDP, thread pinning) is
# Linux-only. On other platforms we still build and unit-test every
# platform-independent component; Linux-specific code is guarded by HFT_LINUX.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HFT_LINUX ON)
  message(STATUS "Platform: Linux (full engine enabled).")
else()
  set(HFT_LINUX OFF)
  message(WARNING
    "Platform: ${CMAKE_SYSTEM_NAME} (NOT Linux). "
    "Networking + thread-affinity components are stubbed/disabled. "
    "Use Linux for end-to-end latency measurement. See README.")
endif()

# ---------------------------------------------------------------------------
# Sanity: ASan/UBSan and TSan are mutually exclusive and incompatible with LTO.
# ---------------------------------------------------------------------------
if(HFT_SANITIZE AND HFT_TSAN)
  message(FATAL_ERROR "HFT_SANITIZE and HFT_TSAN cannot be enabled together.")
endif()
if((HFT_SANITIZE OR HFT_TSAN) AND HFT_LTO)
  message(STATUS "Sanitizer build requested; disabling LTO.")
  set(HFT_LTO OFF)
endif()

# ---------------------------------------------------------------------------
# Common compile flags carried by an INTERFACE target.
# Every hft target links against hft_flags.
# ---------------------------------------------------------------------------
add_library(hft_flags INTERFACE)

target_compile_features(hft_flags INTERFACE cxx_std_20)

# Optimization: the constraints require -O3 -march=native -flto for the hot path.
target_compile_options(hft_flags INTERFACE
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-g>)

if(HFT_NATIVE)
  target_compile_options(hft_flags INTERFACE -march=native)
endif()

if(HFT_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    target_compile_options(hft_flags INTERFACE -flto)
    target_link_options(hft_flags INTERFACE -flto)
  else()
    message(WARNING "LTO requested but not supported by toolchain: ${_ipo_msg}")
  endif()
endif()

# Strict warnings everywhere.
target_compile_options(hft_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wunused -Woverloaded-virtual -Wdouble-promotion
  -Wnull-dereference)
if(HFT_WERROR)
  target_compile_options(hft_flags INTERFACE -Werror)
endif()

# Sanitizers (applied to non-hot-path test builds).
if(HFT_SANITIZE)
  target_compile_options(hft_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hft_flags INTERFACE -fsanitize=address,undefined)
endif()
if(HFT_TSAN)
  target_compile_options(hft_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(hft_flags INTERFACE -fsanitize=thread)
endif()

# Platform define visible to all targets.
if(HFT_LINUX)
  target_compile_definitions(hft_flags INTERFACE HFT_LINUX=1)
else()
  target_compile_definitions(hft_flags INTERFACE HFT_LINUX=0)
endif()

# ---------------------------------------------------------------------------
# Core library.
#
# The engine is largely header-only (templates, inline hot-path code) under
# include/hft. Non-header translation units live in src/ and are added to
# this library as phases land. For now it is an INTERFACE (header) target with
# the include path and flags attached.
# ---------------------------------------------------------------------------
add_library(hft INTERFACE)
target_include_directories(hft INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(hft INTERFACE hft_flags)

# Threads (pthreads) — needed once threading/affinity lands.
find_package(Threads REQUIRED)
target_link_libraries(hft INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(HFT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HFT_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(HFT_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(HFT_BUILD_APPS)
  add_subdirectory(apps)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "==================== hft_engine configuration ====================")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  -march=native   : ${HFT_NATIVE}")
message(STATUS "  LTO             : ${HFT_LTO}")
message(STATUS "  Warnings=errors : ${HFT_WERROR}")
message(STATUS "  ASan+UBSan      : ${HFT_SANITIZE}")
message(STATUS "  TSan            : ${HFT_TSAN}")
message(STATUS "  Linux engine    : ${HFT_LINUX}")
message(STATUS "==================================================================")
