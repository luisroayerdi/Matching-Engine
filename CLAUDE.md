# CLAUDE.md — Matching Engine Project Steering Document

This file is the authoritative guide for every Claude Code session working on this codebase.
Read it fully before writing a single line of code. It is not optional.

---

## Project Identity

A stock exchange matching engine conforming to IEX DEEP Core specifications. The goal is
systems engineering excellence — the finance theme is a vehicle for exploring state-of-the-art
modern C++ for performant, safe, and well-structured systems code.

**This is not a toy.** Every design decision should be defensible in a production context.

---

## Platform Strategy

### Primary Development Platform: macOS (Apple Silicon + x86-64)

Development happens on macOS. The codebase must compile and all tests must pass on macOS.
Benchmarks run on macOS and are meaningful for the matching core, but IPC latency numbers
on macOS are informational only — macOS does not expose user-space real-time scheduling
(`SCHED_FIFO` is restricted to kernel/realtime audio threads), `MAP_HUGETLB`, or
`SO_BUSY_POLL`. Do not write code that assumes these exist unconditionally.

### Future Deployment Platform: Linux x86-64

Linux-specific optimizations (huge pages, CPU pinning, `SCHED_FIFO`, `CLOCK_MONOTONIC_RAW`,
`madvise(MADV_HUGEPAGE)`) are implemented behind the `IEX_PLATFORM_LINUX` compile-time
guard. They must compile on Linux and be compiled out on macOS. They are not stubs —
write the real Linux implementation inside the guard.

### Platform Abstraction Layer: `platform/`

Every OS-specific primitive lives in `platform/`. Code outside `platform/` must never
`#ifdef __APPLE__` or `#ifdef __linux__` directly. It calls through the platform API.

```cpp
// platform/clock.hpp
namespace iex::platform {
  // Returns nanoseconds. CLOCK_MONOTONIC_RAW on Linux, mach_absolute_time on macOS.
  [[nodiscard]] uint64_t now_ns() noexcept;

  // TSC ticks for hot-path microbenchmarks only, not wall time.
  // Uses __rdtsc() on x86, mrs cntvct_el0 on ARM64.
  [[nodiscard]] uint64_t rdtsc() noexcept;
}

// platform/thread.hpp
namespace iex::platform {
  void pin_thread_to_core(int core_id);       // no-op on macOS, affinity on Linux
  void set_realtime_priority(int priority);    // no-op+warn on macOS, SCHED_FIFO on Linux
}

// platform/memory.hpp
namespace iex::platform {
  [[nodiscard]] void* alloc_huge(size_t bytes); // MAP_HUGETLB on Linux, mmap on macOS
  void free_huge(void* ptr, size_t bytes);
}
```

---

## Non-Negotiable Constraints

### Language Standard: C++23

Use C++23 throughout. Rationale:
- `std::expected<T, E>` — fallible hot-path returns without exceptions or out-params
- `std::flat_map` / `std::flat_set` — cache-friendly ordered containers (2-4× faster
  iteration than `std::map` for typical book depths)
- `std::mdspan` — multi-dimensional views over flat price-level arrays
- Deducing `this` — CRTP-free static polymorphism, zero virtual dispatch
- `std::format` — structured logging without heap allocation at consteval sites
- Ranges pipelines — book queries without heap allocation

Never downgrade to C++20/17 idioms when a C++23 feature is available and appropriate.
If a C++23 feature is missing from the minimum compiler version, note it with
`// TODO(C++23): awaiting <feature>` rather than silently downgrading.

### Compiler: Clang only

- **macOS**: Apple Clang 15+ (Xcode 15 minimum). For full `std::flat_map` and
  `std::expected` support, install LLVM Clang 17+ via Homebrew: `brew install llvm`.
  Apple Clang lags one release behind upstream. Check: `clang++ --version` ≥ 17.0.
- **Linux**: Clang 17+ with libc++ (`-stdlib=libc++`)
- **Standard library**: libc++ on both platforms.
  - macOS: system default, do not add `-stdlib=libc++` explicitly (CMake handles it).
  - Linux: `apt install libc++-17-dev libc++abi-17-dev`

Flags that MUST pass with zero warnings:
```
-std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wno-unused-parameter -Werror
```

- `asan` preset: `-fsanitize=address,undefined`
- `tsan` preset: `-fsanitize=thread` (never combined with asan)
- clang-tidy must pass with `.clang-tidy` at repo root

### Build System: CMake + Ninja

- Minimum CMake: 3.28
- All targets in `CMakeLists.txt`; no shell build scripts
- Four presets in `CMakePresets.json`:
  - `debug`   — `-O0 -g3`, asan+ubsan
  - `release` — `-O3 -march=native -flto=thin`
  - `bench`   — release flags + `-fno-omit-frame-pointer` (Instruments / perf)
  - `tsan`    — `-O1 -g`, thread sanitizer
- Dependencies via vcpkg manifest mode (`vcpkg.json`)
- No `find_package(Boost)` — use C++23 or Abseil alternatives first

---

## Latency Targets and Performance Model

### The macOS limitations

The XNU kernel does not support user-space real-time scheduling, huge pages, or busy-poll.
macOS benchmarks measure **algorithmic quality**. Linux benchmarks measure **deployment latency**.
Both matter; they are not the same number.

| Benchmark | macOS Target (p99) | Linux Target (p99) | What It Measures |
|---|---|---|---|
| `BM_OrderInsert` | < 300 ns | < 200 ns | Single add-order, no match |
| `BM_MatchTwoSided` | < 800 ns | < 500 ns | Aggressive crosses resting |
| `BM_SeqlockRead` | < 50 ns | < 20 ns | BBO read under concurrent write |
| `BM_SpscEnqueue` | < 100 ns | < 50 ns | Ring buffer write |
| `BM_SpscRoundtrip` | < 300 ns | < 150 ns | Enqueue + dequeue |
| `BM_FullPipeline` | < 10 µs | < 5 µs | Ingress → match → shm publish |

A 20% regression from the established platform baseline blocks a PR.

### Hot Path Rules (both platforms)

| Concern | Rule |
|---|---|
| Heap allocation in hot path | **Forbidden.** Pre-allocate everything at startup. |
| `std::mutex` / `condition_variable` in hot path | **Forbidden.** Lock-free or seqlock only. |
| `std::shared_ptr` in hot path | **Forbidden.** Pool-allocated raw pointers, explicit ownership. |
| Virtual dispatch in hot path | **Forbidden.** CRTP, concepts, or deducing-this. |
| System calls in hot path | **Forbidden** except `platform::now_ns()`. |
| Exceptions in hot path | **Forbidden.** `std::expected<T, E>` for all fallible operations. |
| `std::cout` / `printf` in hot path | **Forbidden.** Async lock-free logger only. |
| `double` / `float` for prices | **Forbidden.** Fixed-point `int64_t` only. |

"Hot path" = any code reachable from `Sequencer::dispatch()` through `MarketDataPublisher::publish()`.

---

## Architecture Overview

```
                ┌─────────────────────────────────────────────────┐
                │                  Engine Process                  │
                │                                                  │
  Order Input ──►  Sequencer  ──►  Matching Engine  ──►  Book     │
  (POSIX MQ /      (assigns          (price-time         (per-     │
   UDP / pipe)      seq nums)         priority)          symbol)   │
                │       │                  │                │      │
                │       ▼                  ▼                ▼      │
                │  [Seqlock Array]   [Trade Ring]    [Quote Ring]  │
                │       │                  │                │      │
                └───────┼──────────────────┼────────────────┼──────┘
                        │  Shared Memory   │                │
                        │  (shm_open —     │                │
                        │   portable)      │                │
                        ▼                  ▼                ▼
                   Market Data        Trade Feed       Order Book
                   Subscribers        Subscribers      Subscribers
```

### Core Components (implement in this order)

1. **`platform/`** — OS abstraction layer. Implement this **first** so every subsequent
   component is portable by construction.

2. **`engine/types.hpp`** — Domain types. Fixed-width integers only. No `double` for prices.

3. **`engine/order_pool.hpp`** — Fixed-size slab allocator. O(1) alloc/free, zero heap
   traffic after startup.

4. **`engine/order_book.hpp`** — Per-symbol book. `std::flat_map<Price, PriceLevel>`.
   `PriceLevel` = intrusive doubly-linked list of pool-allocated `Order` objects.

5. **`engine/matching_engine.hpp`** — Routes orders, drives matching, emits events.

6. **`ipc/seqlock.hpp`** — Template seqlock for BBO array.

7. **`ipc/spsc_queue.hpp`** — Cache-line-aligned power-of-two SPSC ring. No atomic RMW
   on fast path.

8. **`ipc/shared_segment.hpp`** — RAII `shm_open` + `mmap`. Portable on both platforms.
   macOS: names ≤ 31 chars including `/`. Linux: add `MADV_HUGEPAGE` inside platform guard.

9. **`ipc/sequencer.hpp`** — Single writer to all shared-memory segments.

10. **`market_data/iex_encoder.hpp`** — IEX DEEP 1.0 binary wire format.

11. **`market_data/publisher.hpp`** — Writes to SPSC rings in shared memory.

### IEX Conformance Level

Core DEEP message types, byte-compatible with IEX DEEP 1.0:

| Hex | Type |
|---|---|
| `0x54` | Trade Report |
| `0x51` | Quote Update |
| `0x41` | Add Order |
| `0x44` | Delete Order |
| `0x45` | Order Executed |
| `0x58` | Trading Status |

All messages: 2-byte LE length prefix, 1-byte type, 8-byte POSIX ns timestamp, 8-byte seq num.

---

## Memory Layout Rules

- Every shared-memory struct: `static_assert` size and alignment
- `alignas(64)` on structs accessed by multiple threads
- All shared-memory structs: `std::is_trivially_copyable_v<T>` must be true
- No pointers in shared memory — use indices or offsets
- macOS: shared memory names ≤ 31 chars including `/` (`PSHMNAMLEN` limit)
- Linux: `madvise(MADV_HUGEPAGE)` inside `IEX_PLATFORM_LINUX` guard

---

## macOS-Specific Gotchas

**Know these before writing any platform-adjacent code.**

1. **`shm_open` name limit**: 31 characters including `/`. Use short names:
   `/iex_bbo`, `/iex_trades`, `/iex_quotes`.

2. **No `MAP_HUGETLB`**: Does not exist on macOS. `platform::alloc_huge()` falls back
   to standard `mmap`. This is expected — document it, don't work around it with hacks.

3. **No `pthread_setaffinity_np`**: macOS deprecated thread affinity. `pin_thread_to_core()`
   is a logged no-op on macOS. Benchmark variance will be higher — this is expected.

4. **No `CLOCK_MONOTONIC_RAW`**: Use `platform::now_ns()` which wraps `mach_absolute_time`
   on macOS. `mach_absolute_time` is not NTP-adjusted and has nanosecond resolution.

5. **Apple Silicon uses ARM64, not x86**: `__rdtsc()` is an x86 intrinsic and will not
   compile on Apple Silicon. Use `platform::rdtsc()` which uses `mrs x0, cntvct_el0`
   on ARM64. **Never call `__rdtsc()` directly.**

6. **ARM64 memory model is weaker than x86 TSO**: Relaxed loads/stores that appear
   safe on x86 due to Total Store Order will race on ARM64. ThreadSanitizer on Apple
   Silicon catches these. A TSan race found only on ARM is a real race — fix it.

7. **`std::flat_map` needs Apple Clang 15+ or LLVM Clang 17+**: If `<flat_map>` is
   missing, this is a setup issue. Install `llvm` via Homebrew. Do not work around
   the missing header by substituting `std::map` — that defeats the point.

---

## Code Style and Quality

### Naming

| Concept | Convention |
|---|---|
| Types, concepts | `PascalCase` |
| Functions, variables | `snake_case` |
| Constants, enumerators | `kCamelCase` |
| Template parameters | `TPascalCase` |
| Private member variables | `trailing_underscore_` |
| Macros (avoid) | `SHOUTING_SNAKE` |

### File Layout

```cpp
#pragma once
// Copyright header
// Includes: <stdlib> → "project" (alphabetical within each group)
// namespace iex { ... }
```

No implementation in headers except templates and `inline` functions ≤ ~10 lines.

### Error Handling

- **Hot path**: `std::expected<T, EngineError>`. `[[nodiscard]]` everywhere. Never throw.
- **Initialization** (before hot-path threads start): exceptions permitted.
- `EngineError` is `enum class` in `engine/errors.hpp` with a complete string mapping.

### Comments

Explain **why**, not what. The code says what.

```cpp
// BAD: increment sequence number
seq_++;

// GOOD: odd sequence signals write in progress — readers spin until even
seq_.fetch_add(1, std::memory_order_release);
```

Every `memory_order_*` choice gets a one-line justification.

### Atomics Discipline

- Never `memory_order_seq_cst` without articulating why weaker ordering fails.
- Document the happens-before relationship for every `acquire`/`release` pair.
- Seqlock: `memory_order_acquire` on post-read sequence check.
- SPSC: producer `release` on head write; consumer `acquire` on head read.
- Write to the C++ memory model standard — never assume TSO.

---

## Testing Requirements

### Framework: Google Test + Google Benchmark

```
tests/unit/        — fast, portable, no IPC, no threading
tests/integration/ — full pipeline, multi-threaded, shm
tests/bench/       — Google Benchmark microbenchmarks
```

### Required Tests Per Component

1. Happy path
2. Boundary conditions (empty book, full pool, zero quantity)
3. Concurrent access — validate with TSan preset

### Sanitizer Gates

- `debug` preset (asan+ubsan): zero errors, macOS and Linux
- `tsan` preset: zero data race reports, macOS and Linux
- TSan on Apple Silicon (ARM64) catches races that x86 TSO hides. This is a feature.

---

## What Claude Code Must Never Do

1. **Never `#ifdef __APPLE__` or `#ifdef __linux__` outside of `platform/`.**

2. **Never allocate on the heap in any code reachable from the matching loop.**

3. **Never use `std::mutex` or any blocking primitive on the hot path.**

4. **Never use `double` or `float` for prices or quantities.**

5. **Never silently ignore a `std::expected` error.** Handle both branches explicitly.

6. **Never write a pointer into a shared-memory struct.** Use indices or offsets.

7. **Never leave `memory_order_relaxed` on a cross-thread variable without a comment
   explaining the synchronization contract.**

8. **Never skip `static_assert` size/alignment checks on shared-memory structs.**

9. **Never use `auto` to obscure a semantically meaningful type.**
   `auto price = ...` is bad. `Price price = ...` is correct.

10. **Never break the build.** All four presets must compile on macOS at every commit.
    Linux-only code inside `IEX_PLATFORM_LINUX` guards must compile cleanly on macOS
    (excluded by the guard, not broken syntax).

11. **Never call `__rdtsc()` directly.** Use `platform::rdtsc()`. Breaks Apple Silicon.

12. **Never assume x86 TSO memory ordering.** Write to the C++ memory model.

---

## Suggested Implementation Order

```
Phase 1 — Platform layer + Foundation
  [ ] platform/clock.hpp/cpp   (now_ns, rdtsc — ARM64 + x86 both)
  [ ] platform/thread.hpp/cpp  (pin_thread_to_core, set_realtime_priority)
  [ ] platform/memory.hpp/cpp  (alloc_huge, free_huge)
  [ ] engine/types.hpp         + static_assert battery
  [ ] engine/errors.hpp
  [ ] engine/order_pool.hpp    + unit tests
  [ ] engine/order_book.hpp    (add/cancel/reduce) + unit tests
  [ ] engine/matching_engine.hpp (price-time priority) + unit tests
  [ ] BM_OrderInsert, BM_MatchTwoSided  ← macOS baseline established here

Phase 2 — IPC primitives
  [ ] ipc/seqlock.hpp          + TSan test
  [ ] ipc/spsc_queue.hpp       + TSan test
  [ ] ipc/shared_segment.hpp   (shm_open/mmap RAII, portable)
  [ ] ipc/sequencer.hpp
  [ ] BM_SeqlockRead, BM_SpscEnqueue, BM_SpscRoundtrip

Phase 3 — Market data
  [ ] market_data/iex_encoder.hpp  (wire format, byte-accurate)
  [ ] market_data/publisher.hpp
  [ ] Integration test: order → match → IEX message in shared memory

Phase 4 — IEX compliance validation
  [ ] Decode + validate all required message types vs IEX DEEP 1.0 spec
  [ ] Sequence gap detection test
  [ ] BM_FullPipeline

Phase 5 — Linux hardening (requires Linux machine)
  [ ] All tests pass on Linux
  [ ] Enable platform::pin_thread_to_core  (pthread_setaffinity_np)
  [ ] Enable platform::alloc_huge          (MAP_HUGETLB / MADV_HUGEPAGE)
  [ ] Enable platform::set_realtime_priority (SCHED_FIFO)
  [ ] Full benchmark suite on Linux, compare to Linux targets
  [ ] clang-tidy clean pass on Linux
```

---

## Repository Layout

```
/
├── CLAUDE.md
├── README.md
├── DESIGN.md
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .clang-tidy
├── .clang-format
├── docs/
│   ├── iex-deep-1.0.pdf
│   └── architecture.md
├── platform/
│   ├── clock.hpp / clock.cpp
│   ├── thread.hpp / thread.cpp
│   └── memory.hpp / memory.cpp
├── engine/
│   ├── types.hpp
│   ├── errors.hpp
│   ├── order_pool.hpp
│   ├── order_book.hpp
│   └── matching_engine.hpp
├── ipc/
│   ├── seqlock.hpp
│   ├── spsc_queue.hpp
│   ├── shared_segment.hpp
│   └── sequencer.hpp
├── market_data/
│   ├── iex_encoder.hpp
│   └── publisher.hpp
├── tools/
│   └── order_injector.cpp
└── tests/
    ├── unit/
    ├── integration/
    └── bench/
```

---

## Quick Reference: Key Types

```cpp
// All in namespace iex::

using Price      = int64_t;   // Fixed-point, 4dp. $1.00 = 10'000. $0.0001 = 1.
using Quantity   = uint32_t;  // Share count.
using OrderId    = uint64_t;  // Engine-assigned, monotonically increasing.
using SymbolId   = uint16_t;  // Index into symbol table. Max 65535 symbols.
using SeqNum     = uint64_t;  // IEX sequence number, 1-based.

enum class Side          : uint8_t { kBuy = 'B', kSell = 'S' };
enum class OrderType     : uint8_t { kLimit = 'L', kMarket = 'M', kIOC = 'I' };
enum class TradingStatus : uint8_t { kHalted = 'H', kQuoteOnly = 'Q', kTrading = 'T' };
```

---


