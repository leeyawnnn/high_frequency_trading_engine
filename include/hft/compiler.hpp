// Compiler/architecture intrinsics and hints shared across the hot path.
// Kept tiny and dependency-free so it can be included anywhere.
#pragma once

#include <cstddef>

// ---- Inlining -------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#define HFT_ALWAYS_INLINE inline __attribute__((always_inline))
#define HFT_NOINLINE __attribute__((noinline))
#define HFT_RESTRICT __restrict__
#else
#define HFT_ALWAYS_INLINE inline
#define HFT_NOINLINE
#define HFT_RESTRICT
#endif

// ---- Branch hints (expression-level; complements C++20 [[likely]]) --------
#if defined(__GNUC__) || defined(__clang__)
#define HFT_LIKELY(x) (__builtin_expect(!!(x), 1))
#define HFT_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define HFT_LIKELY(x) (x)
#define HFT_UNLIKELY(x) (x)
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

// ---- Optimisation barriers (benchmark use) ---------------------------------
//
// A microbenchmark measures what the compiler leaves behind, not what the
// source says. If a result is never observed, the computation that produced it
// is dead code and gets deleted, and the loop then measures an empty loop.
// That is how a binary message parse comes to "cost" 0.48 ns, roughly 1.5
// cycles, and how a counter read comes to cost less than a single cycle.
//
// `volatile` is the usual folk remedy and is the wrong tool: it constrains the
// variable's storage accesses rather than the surrounding computation, it
// forces a spill that the real code would not have, and what it forbids is
// under-specified across compilers. These two barriers are the standard
// inline-asm forms, the same ones Google Benchmark uses.
//
// DoNotOptimize(x) tells the compiler that x is observed by something outside
// its analysis, so whatever computed x must survive.
// ClobberMemory() tells it that all of memory may have been read or written,
// so pending stores must be materialised before the barrier.

#if defined(__GNUC__) || defined(__clang__)

template <typename T>
HFT_ALWAYS_INLINE void DoNotOptimize(const T& value) noexcept {
  asm volatile("" : : "r,m"(value) : "memory");
}

template <typename T>
HFT_ALWAYS_INLINE void DoNotOptimize(T& value) noexcept {
#if defined(__clang__)
  asm volatile("" : "+r,m"(value) : : "memory");
#else
  asm volatile("" : "+m,r"(value) : : "memory");
#endif
}

HFT_ALWAYS_INLINE void ClobberMemory() noexcept {
  asm volatile("" : : : "memory");
}

#elif defined(_MSC_VER)

// MSVC has no inline asm on x64. _ReadWriteBarrier is a compiler-level barrier
// only, which is what is wanted here, but it is deprecated and weaker than the
// asm forms: treat MSVC benchmark numbers as indicative rather than
// authoritative.
extern "C" void _ReadWriteBarrier();
#pragma intrinsic(_ReadWriteBarrier)

template <typename T>
HFT_ALWAYS_INLINE void DoNotOptimize(const T& value) noexcept {
  volatile const T* sink = &value;
  (void)sink;
  _ReadWriteBarrier();
}

template <typename T>
HFT_ALWAYS_INLINE void DoNotOptimize(T& value) noexcept {
  volatile T* sink = &value;
  (void)sink;
  _ReadWriteBarrier();
}

HFT_ALWAYS_INLINE void ClobberMemory() noexcept {
  _ReadWriteBarrier();
}

#else
#error "hft/compiler.hpp: no optimisation barrier available for this compiler"
#endif

}  // namespace hft
