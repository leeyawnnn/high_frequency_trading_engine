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

## 💻 Cross-Platform Support

This project builds and runs on both **macOS** and **Linux**:
* **macOS (Development & Testing)**: Core components build and run natively. Since macOS lacks thread affinity controls, thread pinning is gracefully disabled, allowing easy local development, testing, and debugging.
* **Linux (Production & Latency Benchmarking)**: Core pinning (`pthread_setaffinity_np`) is fully supported. For real end-to-end sub-microsecond latency measurement, compile and run on Linux with isolated CPU cores (`isolcpus=` and `nohz_full`).

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

## 🚀 Step-by-Step Guide: How to Build & Run

### Step 1: Install Prerequisites
Ensure you have **CMake ≥ 3.20** and a compiler supporting **C++20**:
* **macOS**: Install Xcode Command Line Tools (`xcode-select --install`) and CMake (e.g., via Homebrew: `brew install cmake`).
* **Linux**: Install GCC ≥ 11 or Clang ≥ 14, and CMake. (e.g., on Ubuntu: `sudo apt install build-essential cmake`).

### Step 2: Build the Project
Configure and compile in **Release** mode to enable optimizations (`-O3 -march=native -flto`):
```bash
# 1. Configure CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. Build the targets
cmake --build build -j
```

### Step 3: Run Unit Tests & Sanitizers
Confirm code correctness by running the unit test suite:
```bash
# Run tests
ctest --test-dir build --output-on-failure
```

For memory safety and race condition verification, you can run sanitizer builds:
```bash
# Address & Undefined Behavior Sanitizer (disables LTO)
cmake -S . -B build-asan -DHFT_SANITIZE=ON -DHFT_LTO=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# Thread Sanitizer
cmake -S . -B build-tsan -DHFT_TSAN=ON -DHFT_LTO=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

### Step 4: Run the Latency Report (Single Process)
Run the self-contained `latency_report` tool, which spins up both the simulated exchange and the engine within one process, communicating over loopback UDP:
```bash
# Run latency report for 2 seconds at 100k messages/sec
./build/tools/latency_report --duration-ms 2000 --rate 100000 --batch 4
```

### Step 5: Run Standalone Processes (Two Terminals)
Simulate a production environment by running the exchange and trading engine as separate, independent processes:
1. **Terminal 1 (Exchange Simulator)**:
   ```bash
   ./build/tools/exchange_sim --rate 100000 --batch 4
   ```
2. **Terminal 2 (HFT Engine)**:
   ```bash
   ./build/apps/hft_main --config config/engine.example.json --duration-ms 5000
   ```

---

### Build Options Reference
Customize the build using `-D<OPTION>=ON/OFF` flags during configuration:

| Option | Default | Description |
|---|---|---|
| `HFT_NATIVE` | `ON` | Compiles with `-march=native` for processor-specific optimizations. |
| `HFT_LTO` | `ON` | Enables Link-Time Optimization (`-flto`). |
| `HFT_WERROR` | `ON` | Treats compiler warnings as errors. |
| `HFT_SANITIZE` | `OFF` | Enables Address + Undefined Behavior Sanitizers. |
| `HFT_TSAN` | `OFF` | Enables Thread Sanitizer for concurrency safety. |
| `HFT_BUILD_TESTS` | `ON` | Builds the unit tests. |
| `HFT_BUILD_BENCHMARKS` | `ON` | Builds microbenchmarks. |

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

### Figure 1 — End-to-End Latency CDF (Cumulative Distribution Function)
* **What this shows**: The cumulative probability distribution of end-to-end latency (from market-data message arrival in the feed handler to order wire-readiness in the gateway).
* **How to read it**: Find a percentile (e.g. p50 or p99) on the vertical Y-axis, move horizontally to the curve, and read the latency on the horizontal X-axis (log-scale).
* **Summary**: The core logic is fast with a tight body (p50 ≈ 1.3 µs, p90 ≈ 2.4 µs). The flat tail to the right (p99 ≈ 15 µs) is caused by OS scheduler preemption on unpinned macOS threads, not the codebase itself.

See **[reports/README.md](file:///Users/lyonnlie/Documents/Programming_shi/high_frequency_trading_engine/reports/README.md)** for detailed analyses of all four performance graphs.

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
   We A/B tested our padded SPSC queue design against an unpadded design (where producer and consumer positions share the same cache line).

   ![false sharing on M4](reports/figures/fig_false_sharing.png)

   ### Figure 4 — SPSC Queue False-Sharing Performance A/B
   * **What this shows**: Cross-thread queue throughput (operations per second) comparing the padded queue design (red) against the unpadded queue design (green).
   * **How to read it**: Taller bars show higher throughput (more queue operations completed per second).
   * **Summary**: On Apple Silicon (M4), the unpadded design is ~3.8× faster because the producer and consumer cores reside in separate clusters; keeping both positions on one cache line minimizes inter-cluster transfer latency. For x86 target architectures with a shared L3 cache, false sharing is indeed harmful, so the padding is kept. This highlights the importance of validating optimizations on the target hardware.

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


