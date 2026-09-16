// Thread core-pinning and naming.
//
// Pinning each pipeline stage to its own dedicated core is what makes the
// latency numbers stable: the thread never migrates, its hot data stays warm in
// that core's L1/L2, and (with isolcpus= + nohz_full on the kernel cmdline) the
// scheduler leaves it alone. This is the single most important "ops" step for
// honest measurement.
//
// Pinning is Linux-only (pthread_setaffinity_np). On macOS it is a no-op that
// reports kUnsupportedPlatform -- the platform deliberately does not expose
// hard affinity, so the engine runs unpinned there for development.
//
// Every entry point here returns its outcome and is [[nodiscard]]. That is
// deliberate: a pin that silently fails does not announce itself, it just
// produces latency percentiles with scheduler noise folded into the tail, and
// those numbers look plausible. Callers must decide what to do about a failure,
// and the measurement report records which of these statuses the run had.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <pthread.h>

#if defined(__linux__)
#include <sched.h>
#endif

namespace hft {

enum class AffinityStatus : std::uint8_t {
  kPinned = 0,           // the thread is bound to the requested core
  kUnsupportedPlatform,  // no hard-affinity API (macOS); thread runs unpinned
  kInvalidCore,          // core was negative or outside the cpu_set_t range
  kFailed,               // the platform call refused; see AffinityResult::error
};

[[nodiscard]] inline const char* to_string(AffinityStatus s) noexcept {
  switch (s) {
    case AffinityStatus::kPinned:
      return "pinned";
    case AffinityStatus::kUnsupportedPlatform:
      return "unsupported-platform";
    case AffinityStatus::kInvalidCore:
      return "invalid-core";
    case AffinityStatus::kFailed:
      return "failed";
  }
  return "unknown";
}

struct AffinityResult {
  AffinityStatus status{AffinityStatus::kUnsupportedPlatform};
  // pthread_setaffinity_np returns an error number directly; it does NOT set
  // errno and does NOT return -1. Carrying it here keeps that distinction
  // visible instead of collapsing the reason into a bare false.
  int error{0};

  [[nodiscard]] bool ok() const noexcept { return status == AffinityStatus::kPinned; }
  explicit operator bool() const noexcept { return ok(); }
};

// Pin the calling thread to a single CPU core.
[[nodiscard]] inline AffinityResult pin_current_thread(int core) noexcept {
#if defined(__linux__)
  // Guard before converting. CPU_SET's glibc expansion assigns the argument to
  // a size_t, so handing it a signed int trips -Wsign-conversion, and a
  // negative or oversized core would index outside the set's bit array.
  if (core < 0 || static_cast<unsigned>(core) >= static_cast<unsigned>(CPU_SETSIZE)) {
    return {AffinityStatus::kInvalidCore, 0};
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(core), &set);
  const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
  if (rc != 0) return {AffinityStatus::kFailed, rc};
  return {AffinityStatus::kPinned, 0};
#else
  (void)core;
  return {AffinityStatus::kUnsupportedPlatform, 0};
#endif
}

// Longest thread name Linux accepts, including the terminating NUL. The kernel
// stores this in task_struct::comm, which is TASK_COMM_LEN (16) bytes.
inline constexpr std::size_t kMaxThreadNameBytes = 16;

// Best-effort thread name (visible in `top -H`, perf, gdb).
//
// Names longer than 15 characters are truncated rather than rejected: Linux
// answers an over-long name with ERANGE and leaves the thread unnamed, which
// loses the name entirely for the sake of a few characters. Truncating keeps
// the stage identifiable in a profile.
[[nodiscard]] inline bool set_current_thread_name([[maybe_unused]] const char* name) noexcept {
#if defined(__linux__)
  char buf[kMaxThreadNameBytes];
  const std::size_t len = std::strlen(name);
  const std::size_t n = len < kMaxThreadNameBytes - 1 ? len : kMaxThreadNameBytes - 1;
  std::memcpy(buf, name, n);
  buf[n] = '\0';
  return ::pthread_setname_np(::pthread_self(), buf) == 0;
#elif defined(__APPLE__)
  // Darwin names the calling thread only and has a more generous limit.
  return ::pthread_setname_np(name) == 0;
#else
  return false;
#endif
}

// Convenience for the pipeline thread entry points, which all want the same
// thing: pin, name, and do not let a real pinning failure pass unnoticed.
//
// This one is intentionally not [[nodiscard]]: it discharges the obligation
// itself by warning, so the thread entry points can call it as a statement.
// It still returns the status for the measurement report to record.
//
// kUnsupportedPlatform is not warned about. It is the expected, documented
// state of every macOS run, and warning three times per start would train the
// reader to ignore the warning that matters. The run's pinning status is
// reported once, in the measurement report header.
inline AffinityResult pin_and_name_thread(int core, const char* name) noexcept {
  const AffinityResult r = pin_current_thread(core);
  (void)set_current_thread_name(name);
  if (r.status == AffinityStatus::kFailed || r.status == AffinityStatus::kInvalidCore) {
    std::fprintf(stderr, "[hft] warning: thread '%s' not pinned to core %d: %s (error %d).\n", name,
                 core, to_string(r.status), r.error);
    std::fprintf(stderr, "[hft] latency tails from this run include scheduler noise.\n");
  }
  return r;
}

}  // namespace hft
