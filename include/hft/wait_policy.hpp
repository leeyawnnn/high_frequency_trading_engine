// What a pipeline thread does when it has no work.
//
// The engine's stages are poll loops: ask the queue (or the socket) for work,
// and if there is none, go round again. How that "go round again" is spelled is
// not a detail -- it is the difference between a latency profile and a lie.
//
// A pure spin is the right answer when the thread owns its core: with
// isolcpus= and nohz_full, nothing else is runnable there, so burning the core
// costs nothing and the thread is already in the pipeline when work lands,
// saving the wake-up. That is the deployment this engine is written for.
//
// A pure spin is the wrong answer everywhere else. On a shared machine -- a
// laptop, a container, a 2-vCPU CI runner -- the producer and consumer are
// runnable on the same core, and a spinning consumer holds the CPU until the
// scheduler preempts it, while the producer that would have given it work sits
// in the run queue. Throughput collapses by orders of magnitude rather than
// degrading gracefully. This is not hypothetical: it is why the SPSC stress
// test ran in 0.7s on a dev box and timed out past 120s on a CI runner.
//
// So the policy is chosen at runtime from the fact that decides it -- whether
// the thread actually got pinned. A thread that asked for a core and got it
// spins. A thread that did not yields after a short spin, which keeps it
// responsive when work is arriving steadily but lets the other side run when
// it is not.
#pragma once

#include <cstdint>
#include <thread>

#include "hft/compiler.hpp"

namespace hft {

// Architectural "I am spinning" hint. On x86 this is PAUSE, which de-pipelines
// the spin and avoids the memory-order-violation pipeline flush when the loop
// finally exits. On AArch64 it is YIELD, an SMT scheduling hint. Neither is a
// system call and neither yields the OS timeslice.
HFT_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  // No portable equivalent; the compiler barrier at least stops the loop body
  // being hoisted out.
  __asm__ __volatile__("" ::: "memory");
#endif
}

// Number of relax iterations before an unpinned thread gives the core back.
// Small enough that a starved peer runs promptly, large enough that a steady
// arrival stream is served without ever reaching the yield.
inline constexpr std::uint32_t kSpinsBeforeYield = 64;

// How a stage should behave when it finds no work.
enum class WaitPolicy : std::uint8_t {
  // Spin iff the thread actually got the core it asked for. The default:
  // right on an isolated core, and safe on a shared one.
  kAdaptive = 0,
  // Always spin. Lowest latency when a core is genuinely free, and the policy
  // a measurement run should use deliberately -- but it collapses throughput
  // on an oversubscribed machine, so it is opt-in rather than implied.
  kSpin,
  // Always back off. Useful when the engine shares cores with other work and
  // latency matters less than not starving the rest of the box.
  kYield,
};

[[nodiscard]] inline const char* to_string(WaitPolicy p) noexcept {
  switch (p) {
    case WaitPolicy::kAdaptive:
      return "adaptive";
    case WaitPolicy::kSpin:
      return "spin";
    case WaitPolicy::kYield:
      return "yield";
  }
  return "unknown";
}

class WaitStrategy {
 public:
  // `pinned` should be the actual outcome of the pin request, not the
  // intention. Asking for a core and not getting one is exactly the case where
  // spinning does damage.
  WaitStrategy(WaitPolicy policy, bool pinned) noexcept
      : spin_only_(policy == WaitPolicy::kSpin || (policy == WaitPolicy::kAdaptive && pinned)) {}

  explicit WaitStrategy(bool pinned) noexcept : WaitStrategy(WaitPolicy::kAdaptive, pinned) {}

  // Call when a poll found no work.
  HFT_ALWAYS_INLINE void idle() noexcept {
    if (spin_only_) {
      cpu_relax();
      return;
    }
    if (++spins_ < kSpinsBeforeYield) {
      cpu_relax();
      return;
    }
    spins_ = 0;
    std::this_thread::yield();
  }

  // Call when a poll found work, so a later idle spell starts its spin budget
  // from the top instead of yielding immediately.
  HFT_ALWAYS_INLINE void reset() noexcept { spins_ = 0; }

  [[nodiscard]] bool spins_only() const noexcept { return spin_only_; }

 private:
  bool spin_only_;
  std::uint32_t spins_{0};
};

}  // namespace hft
