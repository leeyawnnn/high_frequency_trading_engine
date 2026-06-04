// Compiler/architecture intrinsics and hints shared across the hot path.
// Kept tiny and dependency-free so it can be included anywhere.
#pragma once

#include <cstddef>

// ---- Inlining -------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#  define HFT_ALWAYS_INLINE inline __attribute__((always_inline))
#  define HFT_NOINLINE __attribute__((noinline))
#  define HFT_RESTRICT __restrict__
#else
#  define HFT_ALWAYS_INLINE inline
#  define HFT_NOINLINE
#  define HFT_RESTRICT
#endif

// ---- Branch hints (expression-level; complements C++20 [[likely]]) --------
#if defined(__GNUC__) || defined(__clang__)
#  define HFT_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define HFT_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define HFT_LIKELY(x)   (x)
#  define HFT_UNLIKELY(x) (x)
#endif

namespace hft {

// Cache-line size used for false-sharing padding.
//
// The deployment target is Linux/x86-64 (64-byte lines). Apple Silicon uses
// 128-byte lines, so we over-pad there during local development — harmless for
// correctness, and it keeps false-sharing genuinely avoided on this dev box.
// We hardcode rather than use std::hardware_destructive_interference_size to
// avoid the ABI-stability warning that constant carries on several compilers.
#if defined(__aarch64__) && defined(__APPLE__)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

// Software prefetch into L1 (locality 3 = high temporal locality).
HFT_ALWAYS_INLINE void prefetch_read(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, 0 /*read*/, 3 /*high locality*/);
#else
  (void)p;
#endif
}

HFT_ALWAYS_INLINE void prefetch_write(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, 1 /*write*/, 3 /*high locality*/);
#else
  (void)p;
#endif
}

}  // namespace hft
