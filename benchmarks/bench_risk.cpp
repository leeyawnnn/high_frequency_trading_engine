// Benchmark: risk-gate added latency.
//
// We measure the all-checks-pass path (the common case) since that is the
// latency the gate adds to every order that actually goes out. The position
// limit is kept generous and fills net to zero so we never reject; the rate
// limiter is exercised in a separate high-rate configuration.
#include "hft/itch_message.hpp"
#include "hft/order_msg.hpp"
#include "hft/risk_gate.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// Parse an iteration count, refusing anything that is not a positive integer.
long parse_iterations(const char* s) {
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v <= 0) {
    std::fprintf(stderr, "invalid iteration count '%s'; expected a positive integer\n", s);
    std::exit(2);
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  hft::tsc_calibrate();
  // atoi cannot report a bad argument: it returns 0, which would run an empty
  // loop and report an impressive ns/op for having done nothing.
  const long n = (argc > 1) ? parse_iterations(argv[1]) : 50'000'000L;

  std::atomic<bool> kill{false};
  // Generous limits so the accept path is what we measure — but a finite rate
  // (100 M/s) with an enormous burst so the GCRA token bucket DOES run its
  // tsc_now() every check (measuring the real full path) yet never rejects
  // within this loop.
  hft::RiskGate g(hft::RiskLimits{std::int64_t{1} << 40, 1'000'000, 100'000'000.0, 2'000'000'000u},
                  kill);

  // Alternate buy/sell so net position oscillates and never hits the bound.
  std::vector<hft::OrderRequest> orders(2);
  for (int s = 0; s < 2; ++s) {
    orders[static_cast<std::size_t>(s)].side =
        static_cast<std::uint8_t>(s == 0 ? hft::Side::kBuy : hft::Side::kSell);
    orders[static_cast<std::size_t>(s)].size = 1;
  }

  std::uint64_t sink = 0;
  const std::uint64_t t0 = hft::tsc_now_serialized();
  for (long i = 0; i < n; ++i) {
    const hft::OrderRequest& o = orders[static_cast<std::size_t>(i & 1)];
    hft::DoNotOptimize(o);
    sink += static_cast<std::uint64_t>(g.check(o));
    hft::DoNotOptimize(sink);
  }
  const std::uint64_t t1 = hft::tsc_now_serialized();

  const double ns = hft::tsc_to_ns(t1 - t0);
  std::printf("risk gate check() [all checks, accept path]\n");
  const double dn = static_cast<double>(n);
  std::printf("  per check : %.2f ns   (target < 100 ns)\n", ns / dn);
  std::printf("  throughput: %.0f M checks/sec\n", dn / (ns / 1e9) / 1e6);
  std::printf("  accepted  : %llu / %ld\n", static_cast<unsigned long long>(g.accepted()), n);
  return 0;
}
