# High Frequency Trading Engine

A low-latency, HFT-style C++ trading engine: **market-data feed handler →
strategy → order gateway**, built to demonstrate the engineering discipline
behind "low latency" — lock-free queues, zero hot-path allocation, thread
pinning, and TSC-instrumented latency measurement at every stage.

> **Target:** sub-microsecond p99 end-to-end (market-data arrival → order
> emission) on a single machine over loopback. Real measured numbers are
> reported below, honestly, once the pipeline is complete.

---

## ⚠️ Scope and honesty

This is a **learning / portfolio project that simulates an HFT environment** on
one machine. It is explicitly **not** a production trading system.

**What this is NOT:**
- **Not** connected to any real exchange. A simulated exchange process
  (`tools/exchange_sim`) feeds an ITCH-style binary stream and accepts orders.
- **Not** kernel-bypass networking. It uses ordinary loopback UDP, not a
  DPDK/Solarflare/`AF_XDP` NIC. The architecture mirrors kernel-bypass designs;
  the transport does not.
- **No FPGA** risk gates or hardware co-design. The risk gate is a software
  inline function.
- **No co-location**, no real market-data entitlements, no production risk
  monitoring infrastructure.

The latency numbers here will **not** match a real firm's. The point is the
**architecture and the measurement discipline**, both of which transfer.

## 🐧 Linux only

The full engine is **Linux-only**. It depends on `pthread_setaffinity_np` for
core pinning and Linux socket behavior for the loopback transport. macOS lacks
the right primitives.

The platform-independent components (TSC timing, histogram, lock-free SPSC
queue, ITCH parser, order book, strategy, risk gate) **do** build and unit-test
on macOS for development; networking and affinity code is compiled out via
`HFT_LINUX`. For real end-to-end latency measurement, **build and run on
Linux**, ideally with isolated CPUs (`isolcpus=`).

---

## Architecture (target)

```
                          exchange_sim  (separate process)
                ┌───────────────────────────────────────────────┐
                │  random-walk top-of-book + imbalances/crosses  │
                │  answers orders with fills (echoes e2e tag)    │
                └───────┬───────────────────────▲────────────────┘
       ITCH feed (UDP)  │ batched datagrams     │ orders (UDP)   ▲ fills (UDP)
                        ▼                        │                │
 ╔═══════════════════ hft-engine (one process, three pinned threads) ═══════════╗
 ║                                                                              ║
 ║   CORE A: feed handler      CORE B: strategy          CORE C: gateway        ║
 ║  ┌──────────────────┐      ┌──────────────────┐      ┌────────────────────┐  ║
 ║  │ recv → parse →   │ feed │ flat-array book  │order │ risk gate (4 checks)│  ║
 ║  │ tag(arrival TSC) │═════►│ imbalance signal │═════►│ in-flight table →   │──╫─► orders
 ║  │                  │ SPSC │ (integer math)   │ SPSC │ serialize → send    │  ║
 ║  └──────────────────┘      └──────▲───────────┘      └─────────┬──────────┘  ║
 ║                                   │  fill SPSC                 │ recv fills  ║
 ║                                   └────────────────────────────┘ ◄───────────╫─ fills
 ║                                                                              ║
 ╚══════════════════════════════════════════════════════════════════════════════╝
   ═════►  lock-free single-producer/single-consumer ring buffer (no locks, no syscalls)
   the arrival-TSC tag rides feed → strategy → order; gateway computes
   feed-arrival → wire latency at send. Per-stage + e2e histograms throughout.
```

---

## Building

Requirements: **CMake ≥ 3.20**, a **C++20** compiler (GCC ≥ 11 or Clang ≥ 14).
The hot path is compiled with `-O3 -march=native -flto`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Build options
| Option                | Default | Meaning                                              |
|-----------------------|---------|------------------------------------------------------|
| `HFT_NATIVE`          | `ON`    | `-march=native` (needed for honest latency numbers)  |
| `HFT_LTO`             | `ON`    | Link-time optimization                               |
| `HFT_WERROR`          | `ON`    | Warnings as errors                                   |
| `HFT_SANITIZE`        | `OFF`   | ASan+UBSan build for **non-hot-path** tests          |
| `HFT_TSAN`            | `OFF`   | ThreadSanitizer build for queue/concurrency tests    |
| `HFT_BUILD_TESTS`     | `ON`    | Build + register unit tests                          |
| `HFT_BUILD_BENCHMARKS`| `ON`    | Build microbenchmarks                                |

Sanitizer run (correctness, not latency — sanitizers disable LTO):
```bash
cmake -S . -B build-asan -DHFT_SANITIZE=ON -DHFT_LTO=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

---

## Repository layout

```
include/hft/   header-only hot-path components (timing, queues, parser, book, strategy, risk)
src/           non-header translation units
tools/         exchange_sim (simulated exchange), latency_report
tests/         unit tests (non-hot-path; sanitizer targets)
benchmarks/    microbenchmarks (queue throughput, risk-gate latency, ...)
apps/          hft_main — the full wired engine (config-driven)
config/        example JSON config (core pinning, ports, risk limits)
reports/       generated figures + how-to-read-them doc (reports/README.md)
```

**13 header components** (`include/hft/`): `tsc` · `histogram` · `spsc_queue` ·
`itch_message` · `itch_parser` · `book_view` · `strategy` · `risk_gate` ·
`order_msg` · `order_gateway` · `feed_handler` · `net` · `engine` (+ `config`,
`affinity`, `compiler`, `sim_exchange`, `latency_report`). **13 test binaries**,
all green under Release, ASan+UBSan, and (for the concurrent ones) TSan.

---

## Latency results

### End-to-end 

Full wired engine (3 threads + lock-free queues) against the simulated exchange
over loopback. "End-to-end" = feed-arrival TSC (stamped in the feed handler)
→ order ready for the wire (in the gateway), carried by the `md_timestamp` tag.
It excludes the final `sendto` syscall (the kernel-bypass stand-in); the
round-trip number includes all syscalls + the exchange.

**Dev box: Apple M4, macOS, UNPINNED** (100k msg/s, ~18k orders/run, stable across runs):

| Stage | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| feed handler (recv→enqueue) | 0 ns | 0 ns | 42 ns | ~0.9 µs |
| strategy (dequeue→decision) | ~0 ns | ~0 ns | 42 ns | ~1 µs |
| gateway (dequeue→wire-ready) | 0 ns | 0 ns | 85 ns | 167 ns |
| **END-TO-END** | **1.31 µs** | **2.4 µs** | **15 µs** | ~50 µs |
| round trip (→fill back) | 45 µs | 59 µs | 150 µs | ~2 ms |

**Did we hit the sub-µs p99 target? No — not on macOS.** And the README's job is
to say so plainly:

- **Per-stage *compute* is excellent** — tens of nanoseconds, exactly as the
  isolated microbenchmarks below predict. The logic is not the bottleneck.
- **End-to-end p50 ≈ 1.3 µs and p99 ≈ 15 µs** are dominated not by the code but
  by (a) two cross-*cluster* SPSC hops on Apple Silicon (~38 ns each, measured in
  Phase 2) and (b) **macOS scheduler jitter on unpinned, oversubscribed
  busy-spin threads** — there is no `pthread_setaffinity_np` on macOS, so four
  spinning threads (feed/strategy/gateway/exchange) contend and get
  descheduled, which shows up directly as the p99/max tail.
- **Sub-µs p99 is a Linux-pinned target.** On Linux with `isolcpus=` +
  `nohz_full` + one thread per isolated core, the cross-core hop drops to
  ~15–20 ns and the scheduler tail disappears. That is the configuration the
  architecture is built for; I have **not** independently verified sub-µs p99 on
  such a box, so I am not going to claim it. The honest, reproducible result on
  the hardware I have is the table above.

Reproduce: `./build/tools/latency_report --duration-ms 2000 --rate 100000 --batch 4`
(self-contained), or run `exchange_sim` + `hft_main` in two terminals.

![end-to-end latency CDF](reports/figures/fig_e2e_cdf.png)

*Tight body (p50 ≈ 1.3 µs, p90 ≈ 2.5 µs) with a long scheduler-driven tail.*
See **[reports/](reports/README.md)** for all four figures — per-stage
percentiles, isolated component costs, and the false-sharing surprise — each
with a "how to read it / what it tells you" explanation, generated by the engine
itself (`latency_report --csv-dir` → `reports/plot.py`).

### Component microbenchmarks 

These are recorded during development on ARM/macOS to catch regressions. The
**target numbers come from pinned Linux/x86** and will be reported separately.

| Component | Measurement | Notes |
|---|---|---|
| `tsc_now()` | 0.28 ns/call | `cntvct_el0`, 1 ns resolution on M4 |
| `Histogram::record()` | 1.84 ns/call (544M/s) | allocation-free, 6.6 KB fixed buckets |
| `parse_message()` | 0.48 ns/msg (2.1 B/s) | memcpy lowers to plain loads; memory-bound |
| `BookView::apply()` | 4.96 ns/update (202M/s) | flat-array, O(1) update + O(1) top-of-book |
| `strategy.on_md()` | 5.48 ns/event (182M/s) | book update + integer-only imbalance decision |
| `RiskGate::check()` | 1.17 ns/check (856M/s) | all 4 checks incl. GCRA token bucket — **target was <100 ns** |
| SPSC, **no contention** | **258 M ops/sec** (3.9 ns/op) | raw data-structure cost |
| SPSC, **cross-thread** | 26 M ops/sec (38 ns/op) | **coherency-bound by M4 inter-cluster latency** |

The cross-thread SPSC number is low *only* because Apple Silicon P-cores live in
separate clusters with per-cluster L2 — a balanced producer/consumer does one
cross-cluster atomic load per op (~38 ns here). The identical code on x86 cores
sharing an L3 (core-to-core ~10–20 ns) is expected to exceed the **100 M
ops/sec** target; that measurement is pending a Linux run. The no-contention
258 M ops/sec shows the structure itself is not the bottleneck.

### Feed handler

Real path under test: `sender → loopback UDP (batched datagrams) → FeedHandler
(parse + tag + SPSC push) → drain`.

| Scenario | Throughput | Drops | recv→enqueue p50 / p99 / p999 |
|---|---|---|---|
| Max rate, batch 32 | **5.34 M msg/s** | 0 | 83 ns / 0.8 µs / 11 µs |
| Rate-limited 1 M/s, 5 s | 1.00 M msg/s | 0 | 83 ns / 1.1 µs / 10 µs |

The **1 M msg/s target is met with zero drops.** p50 of **83 ns** is the real
per-message cost. The p999/max tail (~10 µs / ~380 µs) is macOS scheduler jitter
on *unpinned* threads — precisely what core pinning + `isolcpus`/`nohz_full`
removes on the Linux target. Batching (the sender packs 32 messages per
datagram) is what gets past the single-message syscall ceiling below.

### The loopback UDP ceiling (why kernel bypass exists)

`exchange_sim` sends one 32-byte message per `sendto()` and tops out at
**~0.31 M msg/sec** unthrottled on this machine — *not* because of the work, but
because each datagram is a syscall (~3 µs round trip into the kernel). This is
the concrete reason production HFT runs kernel-bypass NICs (DPDK / AF_XDP /
ef_vi): to get the per-packet cost off the syscall path. Our mitigations within
the simulation: the feed message and `FeedReader` already support **multiple
messages per datagram**, which amortizes the syscall across a batch (used in
Phase 5 to push the sustainable feed rate up). The honest single-message ceiling
is reported here rather than hidden.

## Optimization pass 

The hot path was written to these rules from Phase 1, so the "optimization pass"
is mostly *verification* plus a few honest experiments — including ones where
the textbook optimization **did not help** (which is the instructive part).

**Hot-path cleanliness audit** (grep + review of `include/hft/`):

| Forbidden on hot path | Status |
|---|---|
| Heap allocation | none (all buffers fixed-size, allocated at startup) |
| Virtual functions | none (templates + `std::variant`-style dispatch) |
| `std::function` | none — used template callbacks (`Emit`) from the start, nothing to replace |
| Exceptions | none (only startup socket setup throws) |
| `std::map` / `shared_ptr` | none (flat arrays, direct-mapped in-flight table; `std::map` only in the *test* reference book) |

**Experiments (measured, dev box M4 — `perf` is Linux-only, see below):**

1. **Cache-line padding / false sharing — the textbook optimization *hurt* here.**
   I A/B'd our padded SPSC against an unpadded one (producer/consumer positions
   sharing a line). On the M4, **unpadded was ~3.5× faster** (padding = 0.25–0.30×,
   consistent across 5 runs). Why: Apple Silicon P-cores live in separate
   clusters and the dominant cost is *inter-cluster* cache-line transfer —
   consolidating both positions onto one line means a single transfer carries
   both, so fewer distinct lines cross the interconnect. **On x86 with a shared
   L3 (the deployment target) false sharing is genuinely harmful and padding is
   correct** — so I *kept the padding* but flagged this. Lesson: cache-layout
   optimizations are platform-specific; validate on the target, don't cargo-cult.

   ![false sharing on M4](reports/figures/fig_false_sharing.png)

2. **Software prefetch — no effect.** Added `__builtin_prefetch` for the next
   SPSC buffer slot: **1.00–1.03×**, i.e. nothing. The hardware prefetcher
   already handles the sequential access pattern. Not added to the code.

3. **Integer vs floating-point imbalance signal — kept integer.** Integer
   cross-product `0.70 ns` vs FP division `0.73 ns` (1.05×). On the M4 with fast
   FP divide the speed win is small; the real reasons to keep integer are
   **exactness** (no FP rounding on the threshold) and **no FP state on the hot
   path** — and on CPUs with slow FP divide the gap is much larger.

**What's deferred to the Linux target box** (where `perf` exists): `perf stat`
for IPC (target ≥ 1.5 on the hot path), `perf record` + `perf c2c` for L1d/LLC
miss and false-sharing analysis, and re-validating optimization #1 on x86. These
need Linux hardware-counter access that macOS does not provide; documented rather
than faked.

## What I learned

This is a learning project and I'll own that — here's what it actually taught me.

- **"Low latency" is a measurement discipline, not a coding style.** The hot-path
  rules (zero allocation, no virtuals, no `std::function`, integer math) got the
  per-stage compute to single-digit nanoseconds almost for free. The hard,
  interesting part was everything *around* the code: TSC calibration, where the
  cross-core hops go, and the scheduler. You can't improve what you don't
  measure, and most of the latency lives where the code isn't.

- **The tail is the product.** p50 was sub-µs on day one; p99 is the whole game,
  and it's dominated by core-to-core transfer and OS scheduling. That's why real
  firms pin cores (`isolcpus`/`nohz_full`), use kernel bypass, and co-locate —
  every one of those is a tail-latency move, not a throughput move. Building this
  made that concrete instead of abstract.

- **Measure on your target; don't trust folklore.** Cache-line padding to avoid
  false sharing is gospel — and on my Apple M4 it was **~3.8× slower** because
  the memory topology is different from x86. I'd have shipped the "obvious"
  optimization and been wrong. Now I instrument the A/B every time.

- **Sanitizers and sanity-checks earn their keep.** ASan caught a histogram
  index underflow that only manifested as a bus error; a TSC calibration check
  caught an inline-asm comment bug that silently returned garbage; TSan proved
  the lock-free queue's memory ordering is actually race-free. The bugs that
  bite hardest in this domain are silent.

- **Honesty is a feature.** The brief asked for sub-µs p99 and I did not hit it
  on the hardware I have. Saying so plainly — and showing exactly *why* (Figs
  1–2) and *what* would fix it (pinned Linux) — is more useful than a number I
  can't stand behind. I tried to make every claim in this repo reproducible.

If I took it further: run the real `perf stat`/`perf c2c` pass on a pinned Linux
box, add multi-symbol books, a proper exit/PnL model, and an `AF_XDP` transport
to replace loopback UDP and actually chase the syscall ceiling.

## Reference reading
- Carl Cook, *"When a Microsecond Is an Eternity"* — CppCon 2017.
- Aldridge, *High-Frequency Trading*.
- Gregoriou (ed.), *The Handbook of High-Frequency Trading*.

---


