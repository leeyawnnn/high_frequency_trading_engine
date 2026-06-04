# High Frequency Trading Engine

Welcome to the **High Frequency Trading (HFT) Engine** project—a low-latency, high-throughput C++20 trading pipeline designed for sub-microsecond execution path profiling.

This repository implements a complete simulated trading loop: ingestion of binary ITCH market data, real-time order book reconstruction, strategy signaling, inline pre-trade risk checks, and UDP-based order emission.

Key features include:
- **Lock-Free Concurrency**: Inter-thread communication via single-producer/single-consumer (SPSC) ring buffers without system calls or mutex contention.
- **Zero Allocation on Hot Path**: Pre-allocated memory pools and flat data structures to avoid heap allocations, virtual functions, and exception-handling overhead.
- **Hardware-Level Telemetry**: Time Stamp Counter (TSC/`rdtsc`) measurement for nanosecond-precision execution path profiling.
- **Core Affinity Pinning**: Dedicated thread pinning (on supported platforms like Linux) to eliminate context switches and maximize cache residency.

---

## Repository Structure

```
hft-engine/
├── CMakeLists.txt
├── README.md
├── include/hft/
│   ├── tsc.hpp                 (rdtsc + calibration)
│   ├── spsc_queue.hpp          (lock-free SPSC ring buffer)
│   ├── histogram.hpp           (latency histogram)
│   ├── itch_message.hpp        (binary feed message types)
│   ├── itch_parser.hpp         (zero-copy parser)
│   ├── book_view.hpp           (compact LOB for HFT — separate from Project 2's)
│   ├── strategy.hpp
│   ├── risk_gate.hpp
│   ├── order_msg.hpp
│   ├── order_gateway.hpp
│   └── engine.hpp
├── src/
│   └── *.cpp
├── tools/
│   ├── exchange_sim.cpp        (sends ITCH messages, accepts orders)
│   └── latency_report.cpp
├── tests/
│   └── *.cpp
├── benchmarks/
│   └── bench_*.cpp
└── apps/
    └── hft_main.cpp
```

---

## Target Architecture

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
```

---

## 🚀 Step-by-Step Guide: How to Build & Run

### Step 1: Install Prerequisites
Ensure you have **CMake ≥ 3.20** and a compiler supporting **C++20** (e.g., GCC ≥ 11 or Clang ≥ 14).

### Step 2: Build the Project
Configure and compile in **Release** mode to enable optimizations (`-O3 -march=native -flto`):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Step 3: Run Unit Tests
Confirm code correctness by running the unit test suite:
```bash
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

## Latency Results & Performance Figures

### End-to-End Latency Profile

The table below shows example latency statistics under standard test conditions with unpinned threads:

| Stage | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| feed handler (recv→enqueue) | 0 ns | 0 ns | 42 ns | ~0.9 µs |
| strategy (dequeue→decision) | ~0 ns | ~0 ns | 42 ns | ~1 µs |
| gateway (dequeue→wire-ready) | 0 ns | 0 ns | 85 ns | 167 ns |
| **END-TO-END** | **1.31 µs** | **2.4 µs** | **15 µs** | ~50 µs |
| round trip (→fill back) | 45 µs | 59 µs | 150 µs | ~2 ms |

* **Per-stage compute latency** is extremely low (tens of nanoseconds).
* **End-to-end latency tail** is typically dominated by queue-wait times and OS scheduler jitter when threads are not pinned to dedicated CPU cores.
* **Sub-microsecond p99 latency** is achievable under Linux environments configured with isolated cores (`isolcpus` and `nohz_full`) and core affinity pinning enabled.

---

### Performance Figures Explained

#### Figure 1 — End-to-End Latency CDF (Cumulative Distribution Function)
![End-to-End Latency CDF](reports/figures/fig_e2e_cdf.png)
* **What this shows**: The cumulative probability distribution of end-to-end latency (from market-data message arrival in the feed handler to order wire-readiness in the gateway).
* **How to read it**: Find a percentile (e.g., p50 or p99) on the vertical Y-axis, move horizontally to the curve, and read the latency on the horizontal X-axis (log-scale).
* **Summary**: The core logic is fast with a tight body (p50 ≈ 1.3 µs, p90 ≈ 2.4 µs). The flat tail to the right is typically caused by OS scheduler preemption on unpinned threads, rather than processing inefficiencies.

#### Figure 2 — Per-Stage Latency Percentiles
![Per-Stage Latency Percentiles](reports/figures/fig_stage_percentiles.png)
* **What this shows**: Comparison of the latency percentiles (p50, p90, p99) for the individual stages (feed handler, strategy, gateway) and the entire end-to-end flow.
* **How to read it**: Compare bar heights across stages (lower is faster), and compare the sum of stages to the end-to-end bar.
* **Summary**: Processing inside each stage is extremely fast (p99 is under 100 ns), but queue waits and scheduling overhead occupy the bulk of the end-to-end latency on unpinned platforms.

#### Figure 3 — Hot-Path Component Cost (Isolated Microbenchmarks)
![Hot-Path Component Cost](reports/figures/fig_components.png)
* **What this shows**: Latency of core components measured in isolation, single-threaded (no scheduling or queue hops).
* **How to read it**: Shorter bars indicate faster operations.
* **Summary**: All hot-path operations (TSC reading, parsing, order book updates, strategy decisions, and risk-gate checks) run in under 6 nanoseconds. The code logic is never the bottleneck.

#### Figure 4 — SPSC Queue False-Sharing Performance A/B
![SPSC Queue False-Sharing Performance A/B](reports/figures/fig_false_sharing.png)
* **What this shows**: Cross-thread queue throughput (operations per second) comparing the padded queue design (red) against the unpadded queue design (green).
* **How to read it**: Taller bars show higher throughput (more queue operations completed per second).
* **Summary**: On CPU architectures where cores reside in separate clusters, the unpadded design can be faster because putting both producer and consumer positions on one cache line minimizes inter-cluster transfer latency. On standard architectures with a shared L3 cache, false sharing is harmful, and the padded design remains correct.

---

### Component Microbenchmarks

| Component | Measurement | Notes |
|---|---|---|
| `tsc_now()` | 0.28 ns/call | Time stamp counter read |
| `Histogram::record()` | 1.84 ns/call | Allocation-free, fixed-size buckets |
| `parse_message()` | 0.48 ns/msg | Zero-copy ITCH message parser |
| `BookView::apply()` | 4.96 ns/update | Flat-array LOB update |
| `strategy.on_md()` | 5.48 ns/event | Book update + integer-only signaling |
| `RiskGate::check()` | 1.17 ns/check | Pre-trade checks including token bucket rate limits |
| SPSC, **no contention** | **258 M ops/sec** | Raw data-structure cost |
| SPSC, **cross-thread** | 26 M ops/sec | Coherency-bound cross-core queue hops |

---

### Feed Handler Performance

| Scenario | Throughput | Drops | recv→enqueue p50 / p99 / p999 |
|---|---|---|---|
| Max rate, batch 32 | **5.34 M msg/s** | 0 | 83 ns / 0.8 µs / 11 µs |
| Rate-limited 1 M/s | 1.00 M msg/s | 0 | 83 ns / 1.1 µs / 10 µs |

* The **1 M msg/s target is met with zero drops.** 
* The p999/max tail is driven by OS scheduler jitter on unpinned threads, which is mitigated via core isolation and affinity.

---

### The Loopback UDP Ceiling (Why Kernel Bypass Exists)

Unthrottled single-message loopback UDP socket calls (`sendto`) typically top out around **~0.3 M msg/sec** due to system call overhead (~3 µs per call). This is why production systems use kernel-bypass NICs (DPDK / AF_XDP). To mitigate this in software simulation, the feed handler and simulator support packing multiple ITCH messages per UDP datagram, amortizing the syscall cost.

---

## Optimization Audit

The hot path adheres to strict zero-overhead design rules:

| Rule | Status | Details |
|---|---|---|
| Heap allocation | **None** | All buffers and queues are pre-allocated at startup. |
| Virtual functions | **None** | Resolved at compile time via template-based generic dispatch. |
| `std::function` | **None** | Replaced with template callbacks to enable direct compiler inlining. |
| Exceptions | **None** | Sockets throw on setup; hot-path errors use code-based propagation. |
| Associative maps | **None** | Replaced with flat arrays and direct index-mapped tracking tables. |

---

## Key Takeaways

- **"Low latency" is a measurement discipline.** Zero-allocation code and template dispatch keep primitive operations in the single-digit nanoseconds. The bulk of real-world latency is scheduling, context switches, and cache coherency.
- **The tail is the product.** Average latency is easy to optimize; controlling p99/p99.9 outliers requires hardware co-location, core isolation, thread pinning, and bypassing OS kernel networking.
- **Validate optimizations on target hardware.** Cache-line padding to prevent false sharing is standard on x86, but may behave differently on modern multi-cluster topologies. Always profile on the target production hardware.

---

## Reference Reading
* Carl Cook, *"When a Microsecond Is an Eternity"* — CppCon 2017.
* Irene Aldridge, *High-Frequency Trading: A Practical Guide to Algorithmic Strategies and Trading Systems*.
* Gregoriou (ed.), *The Handbook of High-Frequency Trading*.
