# FIXES

What was broken, what changed, which claims were retracted, and which numbers
moved. This file is a working record for review; the parts worth keeping belong
in the README's Limitations section, after which this file should be deleted.

Branch: `fix/portfolio-review`. Baseline: `3556864`.

---

## 1. The repository did not build on its stated target platform

Two compile errors on GCC/Linux under `-Werror`, in code inside
`#if defined(__linux__)` branches that had never been through a compiler
because development happened on macOS.

- `affinity.hpp:29` — `CPU_SET(core, &set)` passed a signed `int` into a macro
  that assigns to `size_t`, tripping `-Werror=sign-conversion`.
- `risk_gate.hpp:34` — the enumerator `RiskResult::kOrderSize` shadowed
  `inline constexpr std::size_t kOrderSize = 40` from `order_msg.hpp`, tripping
  `-Werror=shadow`.

**Additional finding:** the review named two files. There were **three**.
`bench_spsc.cpp` and `exchange_sim.cpp` each carried a private copy of the
pinning helper with the identical `CPU_SET` defect. The duplication was why a
fix in `affinity.hpp` could not reach them; both now call the shared helper.

Two further defects in `affinity.hpp`, neither of which produces a wrong
answer so much as an unverifiable one:

- `pthread_setaffinity_np` returns an error number directly. It does not set
  `errno` and does not return `-1`. The old code collapsed it to a `bool`, so
  "EPERM under a restrictive policy" was indistinguishable from "this platform
  has no affinity API". A pin that silently fails still produces percentiles;
  they just have scheduler noise in the tail and look plausible.
- `set_current_thread_name` passed arbitrary-length names to a call that
  answers anything over 15 characters with `ERANGE` and leaves the thread
  unnamed.

## 2. The test suite could not pass on a shared machine

`test_spsc` pushed a fixed **100,000,000** items with a pure spin on both the
full and empty paths. That is 0.7s on a dev box with two idle cores and over
120s on a CI runner where the producer and consumer contend for one.

Fixed by bounding the case by wall-clock rather than by operation count, and by
giving both threads a backoff policy. Throughput assertions were removed
entirely — throughput is a property of the machine and belongs in
`benchmarks/`.

**Correction to my own earlier analysis:** I first reported that an oversized
op count was *not* the cause, reasoning from the 0.73s local runtime. That was
wrong. The local machine is simply about 100x faster at this than a contended
runner, and the cause is the op count and the pure spin together.

**Separately, and the actual CI blocker:** `test_engine` asserted
`e2e p50 < 1'000'000 ns`, with the assertion compiled out under sanitizers
because they were too slow to meet it. That exemption was the tell — a
threshold that must be disabled in one configuration is measuring the machine.
It failed on every Debug leg and both macOS legs. Replaced with invariants that
hold on any machine.

## 3. Claims retracted

| claim | where | status |
|---|---|---|
| strategy p50 "~0 ns", p90 "~0 ns", p99 "42 ns" | README table | **Retracted.** The committed CSV said 399 / 719 / 1279 ns in the same commit. |
| feed handler p90 "0 ns" | README table | **Retracted.** Data said 42 ns. |
| round trip p50 "45 µs", p90 "59 µs" | README table | **Retracted.** Data said 21.5 / 50.2 µs. |
| gateway p99 "85 ns" | README table | **Retracted.** Data said 42 ns. Not in the original review; found by re-checking every cell. |
| "sub-microsecond execution path profiling" | README intro | **Retracted.** p50s sat at the clock's resolution floor. Restated as a pinned-Linux target that this repository has not demonstrated. |
| "binary ITCH market data", "zero-copy ITCH message parser" | README | **Qualified.** The 32-byte native-endian frame is not ITCH. A real ITCH 5.0 decoder now exists alongside it, and the README distinguishes them. |
| repository tree showing `src/` | README | **Removed.** No `src/` directory exists. |
| implied Windows support | README | **Retracted.** Stated unsupported; the networking and affinity layers are POSIX. |
| "unpadded can be faster on clustered-core architectures" | reports/README | **Upheld, and now reproducible.** It had no producer; now measured at ~4x on this M4, with the caveat that it is AArch64-only. |

## 4. Numbers that changed, and why

All measured on Apple M4, 10 cores, macOS 26.6.1, unpinned, `--wait spin`.

| figure | was | now | why |
|---|---|---|---|
| feed p50 | `0 ns` | `<2 ns` | The counter cannot resolve it. `0` claimed no time elapsed; `<2` states the floor. |
| gateway p50 / p90 | `0 ns` | `<2 ns` | As above. |
| e2e p50 | 1343 ns | 1215 ns | Re-measured. Same order; run-to-run variation. |
| round trip p50 | 21503 ns | 21503 ns | Reproduced exactly, which is what identified the original run's wait policy. |
| `tsc_now` | 0.28 ns | 0.292 ns, upper bound | Not separable from an empty barrier loop at 0.313 ns. Published as a bound, with the baseline beside it. |
| `parse_message` | 0.48 ns | 0.411 ns | Real barriers, and a cache-resident working set. See below. |
| `risk_gate_check` | 1.17 ns | 0.332 ns | The old `volatile` sink cost a spill per iteration. |
| `book_apply` | 4.96 ns | 4.201 ns | As above. Cross-checks against `bench_book` at 4.40 ns. |
| `strategy_on_md` | 5.48 ns | 4.546 ns | As above. |
| padded vs unpadded | 34 / 130 M ops/s | 26.9 / 112.7 M ops/s | Same direction, ~4x, now from a committed producer. |

**The review predicted `parse_message` would get 10–50x worse under real
barriers. It did not, and I did not make it.** The function is a 32-byte
`memcpy` into a packed struct — two vector load/store pairs, roughly two cycles
— so sub-nanosecond is the right order of magnitude. The published figure is
what it measures.

**A measurement bug I introduced and then fixed.** My first component
benchmark generated one message per iteration, so each repetition streamed
64 MB from DRAM and `parse_message` was a memory-bandwidth test: 0.488 ns/msg
on a quiet machine, 1.553 ns/msg minutes later with a browser running. The
inter-quartile range within a run did not catch it, because every repetition in
a run shares the same background load. Only re-running the whole program
against the committed CSV exposed it. The working set is now a fixed 64Ki
messages (2 MB, cache-resident) and three consecutive runs agree to within a
fraction of a percent.

**A process failure worth recording.** A contention experiment left background
spinner processes running, and several benchmark readings were taken while the
machine carried a load average above 200. Those were discarded and re-measured
on an idle machine. The committed numbers are from the settled machine, and
`scripts/measure.sh` now records the load average at the start of every run so
this is visible rather than inferred.

## 5. Real defects found and fixed

**A zero-size Add became the top of book.** `BookView::apply` called
`on_size_increased` for every Add without consulting the size, and that
function promotes the index to best-of-book whenever it is better than the
current best. A zero-size Add at a better price installed an *empty* level as
the top: `has_bid()` true, `best_bid_size()` 0, and `imbalance()` returning a
signal computed from a level holding nothing — which the strategy consumes
directly, so it reaches the order path.

The differential test against the `std::map` reference had run 200,000 messages
without catching it, for two reasons: it compared the books every 1024 messages
rather than after each one, and its generator drew sizes from [1, 2000] and so
never emitted the value that breaks it. Found by adding per-message invariants
that check the book against itself rather than against a reference.

**Four `(double + 0.5)` truncating casts**, including one in `tsc_to_ns_u`, on
the path that converts every measured cycle count into the nanoseconds the
published percentiles are built from. Behaviour is identical for the
non-negative inputs these see today, so no committed number changed; it removes
a trap rather than correcting a figure.

**Unchecked `std::optional` dereferences in `test_strategy`.** `CHECK` records
a failure and continues, so a failed `has_value()` was followed by `*opt`
anyway — undefined behaviour, and a crash instead of a readable assertion. The
harness advertised a `REQUIRE` macro that did not exist; it does now.

**`atoi` in three benchmarks**, which cannot report a parse error and returns
0, so a typo ran a zero-iteration loop and printed a throughput figure for
doing nothing.

## 6. Unreproducible artifacts

`reports/data/components.csv` and `reports/data/false_sharing.csv` had **no
producer anywhere in the repository**. No committed program emitted those
component names or those variant labels. The README's component table and two
of its four figures traced to files of unknown provenance.

This was not in the review, which assumed `bench_book` and `bench_risk`
produced them and asked for barriers to be added. Both producers now exist
(`bench_components`, `bench_false_sharing`), with medians and inter-quartile
ranges across repetitions, a discarded warm-up, and real optimisation barriers.

## 7. Where the review was wrong

Per the standing instruction to say so rather than quietly doing something else:

**The 24 MHz clock diagnosis does not survive the data.** The review attributed
the `p50 = 0 ns` readings to a 24 MHz AArch64 timer (~41.67 ns/tick), reading
42, 83 and 250 in `summary.csv` as multiples of one tick. If that were the tick
period, the only representable values below 300 ns would be 0, 42, 83, 125,
167, 208, 250 and 292. The raw histogram in `feed.csv` holds samples at 17, 18,
58, 59, 60, 101, 143, 187, 211, 227, 251 and 295 ns, which cannot exist on that
counter. The apparent multiples are HdrHistogram bucket boundaries, and the
summary alone cannot distinguish the two.

The counter here reads `cntfrq_el0` = 1.000 GHz, a 1 ns tick, consistent with
both the raw buckets and the Apple M4 the reports already named. **The
conclusion stands and the cause is different:** the instrumented spans are
genuinely shorter than the counter resolves, which is a problem to fix in the
measurement rather than a coarse clock to migrate off.

**The ITCH sample data is reachable, at a different URL.** The review suggested
checking `ftp://emi.nasdaq.com/ITCH/` and preparing a fallback. That endpoint
refuses connections (curl exit 7), but the data moved to
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` and serves full-day files. The
decoder is validated against 359,802 real messages rather than against a
synthetic substitute.

**`parse_message` did not regress**, as above.

**The TODO/placeholder count was 30, not 28.** All removed.

## 8. Still not done

- **No x86 run.** Every number here is from one AArch64 machine. The padding
  result in particular should not be generalised, and the per-stage medians
  that sit at the clock floor would likely become measurable on a machine with
  a sub-nanosecond TSC.
- **Coordinated omission is not corrected.** The load sweep drives at a fixed
  offered rate and measures service time, so those tails are optimistic. The
  correction is understood and unimplemented.
- **No queue-occupancy-over-time figure.** The review asked for one per SPSC
  hop to show where latency accumulates. The latency-vs-load curve covers the
  headline question; this would answer the follow-up.
- **`test_harness.hpp` remains hand-rolled** while sibling repositories use
  Catch2 and GoogleTest. It is now a stated choice — zero dependencies, and the
  harness is about 100 lines — rather than an unexplained inconsistency, but it
  is still an inconsistency.
- **Repo metadata** (description, topics, rename to `hft-engine`) needs the
  GitHub UI or an authenticated `gh`. Proposed values are in the PR
  description.
