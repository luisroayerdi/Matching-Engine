# DESIGN.md — Architecture Decisions & Tradeoff Log

Every significant design decision is recorded here: what was chosen, what was considered,
and the explicit tradeoff accepted. This is a living document — update it when decisions change.

---

## 1. Platform Strategy: macOS Development → Linux Deployment

### Decision
Primary development on macOS (Apple Silicon + x86-64). Deployment target is Linux x86-64.
All OS-specific code lives in `platform/` behind a clean abstraction boundary.

### The macOS Latency Reality (be honest about this)

macOS is not a real-time OS for user processes. Specific limitations:

| Feature | Linux | macOS | Impact |
|---|---|---|---|
| `SCHED_FIFO` user threads | ✓ | ✗ (audio/kernel only) | Scheduler jitter on macOS |
| `pthread_setaffinity_np` | ✓ | ✗ (deprecated) | No core pinning; benchmark variance |
| `MAP_HUGETLB` / `MADV_HUGEPAGE` | ✓ | ✗ | TLB pressure higher on macOS |
| `CLOCK_MONOTONIC_RAW` | ✓ | ✗ | Use `mach_absolute_time` instead |
| `SO_BUSY_POLL` | ✓ | ✗ | Kernel-bypass not available |
| `shm_open` name length | 255 chars | 31 chars | Keep shm names short |

macOS full-pipeline latency will be 2–3× higher than Linux. This is expected and documented.
The macOS benchmark numbers establish **algorithmic baselines**, not deployment targets.

### Why macOS as the dev platform despite this?

1. **Apple Silicon (ARM64) has a weaker memory model than x86**: x86 Total Store Order
   (TSO) prevents many classes of memory ordering bugs from manifesting. ARM64 does not.
   Developing on Apple Silicon means ThreadSanitizer catches races that would silently
   pass on x86 — the bugs surface in development, not production.

2. **Developer ergonomics**: macOS is the daily driver. Forcing Linux-only development
   adds friction (VMs, remote machines) that reduces iteration speed during the
   algorithm-heavy early phases.

3. **The matching core is platform-independent**: The `engine/` components — types, pool,
   book, matching algorithm — have zero OS dependencies. They are identical on both platforms
   and their performance is meaningfully measurable on macOS.

### Platform Abstraction Design

The `platform/` layer uses compile-time dispatch, not runtime polymorphism:

```cpp
// platform/clock.cpp
uint64_t iex::platform::now_ns() noexcept {
#if defined(IEX_PLATFORM_LINUX)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
#elif defined(IEX_PLATFORM_MACOS)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);  // nanosecond-resolution, not NTP-adjusted
#endif
}
```

`IEX_PLATFORM_LINUX` and `IEX_PLATFORM_MACOS` are set by CMake based on `CMAKE_SYSTEM_NAME`.
No code outside `platform/` uses these macros.

---

## 2. Language Standard: C++23

### Decision
C++23 throughout. Minimum Clang 17 (LLVM) or Apple Clang 15 (Xcode 15).

### Features that justify C++23 (not just "nice to have")

**`std::expected<T, E>`** — The most impactful feature for this codebase. The matching
engine has many fallible operations: order submission can fail if the pool is exhausted,
the symbol is unknown, or a price is invalid. Without `std::expected`, the choices are:
- Return codes: type-unsafe, silently ignorable, no value+error simultaneously
- Exceptions: unpredictable latency when thrown, unwind tables inflate icache
- Output parameters: ugly, non-composable

`std::expected` is zero-overhead on the success path (monadic chains compile identically
to manual if-checks), forces callers to handle errors (`[[nodiscard]]`), and returns both
a value and a rich error descriptor in a type-safe sum type.

**`std::flat_map<Price, PriceLevel>`** — The price-level index for each book side.
`std::map` nodes are heap-allocated and pointer-chased — each lookup crosses multiple
cache lines. `std::flat_map` is a sorted vector: binary search over contiguous memory
with hardware prefetch. For a 10-level book, `flat_map` iteration is 2–4× faster.
Insert is O(n) due to shift, but new price levels are rare (most orders join existing
levels), and n is small (< 20 active levels per side in typical workloads).

**Deducing `this`** — Enables CRTP-style static polymorphism without the CRTP syntax:
```cpp
// C++20 CRTP (verbose, error-prone)
template<typename Derived>
struct OrderPolicy { void execute(this Derived& self) { ... } };

// C++23 deducing-this (clean)
struct OrderPolicy {
    template<typename Self>
    void execute(this Self& self) { ... }
};
```
Used for order type dispatch (Limit / Market / IOC) without virtual calls.

### Why not C++26?
C++26 contracts (`[[pre:]]`, `[[post:]]`) would be valuable here, and `std::execution`
would clean up the async publisher design. However, C++26 compiler support is experimental
as of Clang 17. We stay on C++23 and add a `DESIGN.md` note to revisit when Clang 19
stabilizes C++26 support.

---

## 3. Price Representation: Fixed-Point `int64_t`

### Decision
All prices are `int64_t` with 4 implicit decimal places. $1.00 = 10,000. $0.0001 = 1.

### Why not `double`?
1. **Non-determinism across architectures**: IEEE 754 double arithmetic is not bit-identical
   across hardware when FMA (fused multiply-add) is available on one machine and not another.
   On Apple Silicon (M-series), FMA is always available. On some x86 configurations, FMA
   is optional. A matching engine that produces different fill prices on different machines
   is broken.

2. **Comparison fragility**: `price_a == price_b` is unreliable with floating point.
   Price-level equality is the atomic operation of the order book. It must be exact.

3. **Explicit overflow**: `int64_t` overflow is defined (UBSan catches it). `double` silently
   loses precision for prices > 2^53 / 10000 ≈ $900 trillion. The `int64_t` range is sufficient:
   max representable price = INT64_MAX / 10000 ≈ $922 trillion.

### Why 4 decimal places?
IEX quotes prices in dollars with up to 4 decimal places ($0.0001 minimum tick). 4dp
gives exact representation of all legal IEX prices within the `int64_t` range.

---

## 4. Order Book: `flat_map` + Intrusive List

### Decision
`std::flat_map<Price, PriceLevel>` for the price-level index.
`PriceLevel` contains an intrusive doubly-linked list of pool-allocated `Order*`.

### Why intrusive list for intra-level FIFO?
- **Zero allocation**: `Order` contains its own `prev*` and `next*` members. Adding to or
  removing from a price level is pointer surgery — no heap traffic.
- **O(1) cancel**: Given an `Order*` (from the `OrderId → Order*` hash map), cancel is
  O(1): splice out of the intrusive list and return to the pool. No level scan.
- **FIFO preservation**: Price-time priority requires FIFO within a price level.
  An intrusive list preserves insertion order exactly.

### `flat_map` insert O(n) — acceptable?
Yes. New price level creation is rare. When a new price level appears, `flat_map` shifts
elements to maintain sort order. For a book with < 20 active levels and price levels
measured in ticks, shifts are ~20 × 8 bytes = 160 bytes — fits in a cache line or two.
The tradeoff is: rare O(n) insert in exchange for fast O(k) iteration on every BBO update,
market-by-price publish, and cancel-sweep.

---

## 5. Memory Management: Slab Pool Allocator

### Decision
Fixed-size pool of `Order` objects, pre-allocated at startup with `platform::alloc_huge()`.
All order creation/destruction goes through the pool. Zero heap allocation on the hot path.

### Pool Size
Default: 1,000,000 `Order` slots. `Order` is 64 bytes (one cache line). Total: 64 MB.
Configurable at startup. On Linux, backed by 2MB huge pages via `MAP_HUGETLB` to reduce
TLB pressure. On macOS, standard anonymous `mmap` (no huge page support).

### Why not `std::pmr`?
`std::pmr::monotonic_buffer_resource` is one-way (no free). `std::pmr::unsynchronized_pool_resource`
supports free but has no latency guarantees and a complex implementation. Our slab pool
is ~80 lines with deterministic O(1) allocate and free via a free-list.

---

## 6. IPC: Seqlock for Best-Bid/Ask

### Decision
The BBO (best bid and offer) for each symbol is published via a seqlock array indexed by
`SymbolId`. Readers busy-poll the sequence number to detect writes.

### Protocol
```
Writer: seq.fetch_add(1, release)  // odd → write in progress
        [write BBO data]
        seq.fetch_add(1, release)  // even → write complete

Reader: do {
          s1 = seq.load(acquire)
          if (s1 & 1) continue;    // writer active, retry
          [read BBO data]
          s2 = seq.load(acquire)
        } while (s1 != s2);        // writer interrupted, retry
```

### Why seqlock over mutex for BBO?
- Readers never block the writer — a seqlock write is ~5 ns, a mutex cycle is 20–50 ns
  without contention and unbounded with it.
- Multiple readers are fully concurrent, no inter-reader coordination.
- The BBO struct (two prices + two sizes = 32 bytes) fits in one cache line, making the
  data copy inside the seqlock nearly free.

### macOS note
On macOS without core pinning, the seqlock p99 is ~50 ns vs ~20 ns on pinned Linux cores.
The algorithm is identical; the variance comes from the scheduler.

---

## 7. IPC: SPSC Ring Buffer for Trade and Quote Events

### Decision
Separate SPSC (single-producer, single-consumer) ring buffers for trade events and quote
events, in shared memory. Power-of-two size (default 65536 slots).

### Key Design Point: No Atomic RMW on Fast Path
Single producer means we can use a locally cached head counter and publish to the atomic
only when a slot is ready. No `fetch_add`, no CAS. The write path is:
```cpp
uint64_t head = cached_head_++;          // local counter, no atomic
slots_[head & kMask] = event;            // write data
head_.store(cached_head_, release);      // single atomic store to publish
```

### Overflow Policy: Lossy (Oldest-First Overwrite)
On overflow, the oldest slot is overwritten. This is intentional — the engine's latency
must never couple to subscriber performance. Subscribers detect gaps via sequence numbers
and either re-snapshot or report a feed gap. This is the standard market data feed model.

---

## 8. Error Handling: `std::expected<T, E>`

### Decision
All fallible hot-path functions return `std::expected<T, EngineError>` with `[[nodiscard]]`.
Exceptions are permitted only in constructors and initialization code.

### Why not exceptions everywhere?
On the hot path, even zero-cost exception tables have costs:
1. **Unwind table bloat**: Exception metadata inflates binary size, increasing icache pressure.
2. **Unpredictable throw latency**: Unwinding frames and searching tables takes microseconds.

`std::expected` makes error handling local and explicit. The optimizer treats the error
path as `[[unlikely]]` and places it out-of-line, keeping the hot path icache-clean.

---

## 9. ARM64 / Apple Silicon Concurrency Considerations

### The TSO Trap
x86 has Total Store Order: stores are globally visible in program order; loads see the
most recent store from any core. Many "lock-free" algorithms written for x86 rely on TSO
without realizing it — they use `memory_order_relaxed` where `memory_order_release` is
required, and it works on x86 because TSO provides the ordering for free.

ARM64 is weakly ordered. On Apple Silicon, these latent bugs manifest as real data races.
ThreadSanitizer on ARM64 is therefore **more valuable** as a correctness tool than TSan
on x86 — it will find races that x86 TSan misses.

### Rule
Write every atomic operation to the C++ memory model standard. Never rely on TSO.
Every `memory_order_relaxed` on a cross-thread variable requires a comment explaining
the synchronization contract that makes relaxed ordering safe.

### `rdtsc` on ARM64
`__rdtsc()` is an x86 intrinsic. On Apple Silicon, the equivalent is the ARM64 virtual
counter: `mrs x0, cntvct_el0`. `platform::rdtsc()` wraps both. This is used only for
microbenchmark timing, never for wall-clock or sequence-number purposes.

---

## 10. Sequencer as Single Writer

### Decision
The sequencer is the only entity that writes to shared-memory segments.

### Why single writer?
- Eliminates all write-side contention on shared-memory structs
- Sequence number assignment is a simple counter increment (no coordination)
- Cache coherence: a single writer means written cache lines are never bounced between
  CPU cores for the write path, which is the dominant coherence cost

### Tradeoff
The sequencer is a single point of failure and a throughput bottleneck. For this project
(one engine process, development focus), this is the correct tradeoff. A production
system would have sequencer redundancy with primary/backup failover.

---

## Open Questions

- [ ] **Tick-to-trade latency measurement**: We need a hardware timestamp source
  (platform RDTSC or PTP) to measure true ingress→publish latency, not just per-component
  benchmarks. `platform::rdtsc()` is the right hook.

- [ ] **Order book snapshot protocol**: Subscribers joining mid-session need a consistent
  snapshot before consuming the incremental ring. Design: sequencer publishes a
  `SnapshotComplete` message; subscribers discard ring messages before its sequence number.

- [ ] **Session management**: IEX DEEP has sequence number resets at market open. We need
  a `SessionReset` message type and a counter reset mechanism.

- [ ] **Python bindings**: Deferred to post-Phase 4. Will use pybind11. The binding
  will expose `Engine.submit_order()`, `Engine.cancel_order()`, and read-only
  `SharedMemoryReader` accessors for BBO and trade events.

- [ ] **Linux CI**: A Linux runner (GitHub Actions `ubuntu-latest`) should run the full
  test suite including TSan on every PR once Phase 2 is complete.

