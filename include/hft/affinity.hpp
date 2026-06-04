// Thread core-pinning and naming.
//
// Pinning each pipeline stage to its own dedicated core is what makes the
// latency numbers stable: the thread never migrates, its hot data stays warm in
// that core's L1/L2, and (with isolcpus= + nohz_full on the kernel cmdline) the
// scheduler leaves it alone. This is the single most important "ops" step for
// honest measurement.
//
// Pinning is Linux-only (pthread_setaffinity_np). On macOS it is a no-op that
// returns false — the platform deliberately does not expose hard affinity, so
// the engine runs unpinned there for development and the README says so.
#pragma once

#include <pthread.h>

#if defined(__linux__)
#  include <sched.h>
#endif

namespace hft {

// Pin the calling thread to a single CPU core. Returns true on success, false
// if core < 0 or the platform has no affinity API.
inline bool pin_current_thread(int core) {
#if defined(__linux__)
  if (core < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#else
  (void)core;
  return false;
#endif
}

// Best-effort thread name (visible in `top -H`, perf, gdb). Linux caps at 16
// bytes including NUL.
inline void set_current_thread_name([[maybe_unused]] const char* name) {
#if defined(__linux__)
  ::pthread_setname_np(::pthread_self(), name);
#elif defined(__APPLE__)
  ::pthread_setname_np(name);
#endif
}

}  // namespace hft
