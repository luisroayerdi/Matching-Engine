# IEX Matching Engine

A stock exchange matching engine conforming to IEX DEEP 1.0 Core, built as a systems
engineering study in modern C++23. The goal is production-grade lock-free systems: seqlock shared memory, SPSC ring buffers, a
fixed-point matching core, and a Python binding that lets you drive the engine and
visualize market data from a Jupyter notebook.

---

## What This Is

- A **limit order book** with price-time priority matching (limit, market, IOC)
- A **sequencer** that assigns monotonically increasing IEX-compatible sequence numbers
- **Lock-free IPC** via POSIX shared memory, SPSC rings, and seqlock arrays
- A **market data publisher** encoding trades, quotes, and book events into IEX DEEP binary
- A **pybind11 binding** exposing the engine to Python for research and visualization
- A **benchmark suite** with explicit latency targets that gate each phase

## What This Is Not

- A production exchange (no persistence, no failover, no regulatory compliance)
- A full IEX DEEP implementation (core message types only)
- A cross-platform project — macOS only

---

## Platform

macOS (Apple Silicon + x86-64). The project is intentionally single-platform.
Apple Silicon's weak ARM64 memory model makes ThreadSanitizer more aggressive than on
x86 — it catches real data races that x86 TSO silently hides. This is a feature.

---

## Performance Targets (macOS, algorithmic baselines)

| Benchmark | Target (p99) | What It Measures |
|---|---|---|
| `BM_OrderInsert` | < 300 ns | Single add-order, no match |
| `BM_MatchTwoSided` | < 800 ns | Aggressive order crosses resting |
| `BM_SeqlockRead` | < 50 ns | BBO read under concurrent write |
| `BM_SpscEnqueue` | < 100 ns | Ring buffer write |
| `BM_SpscRoundtrip` | < 300 ns | Enqueue + dequeue |
| `BM_FullPipeline` | < 10 µs | Ingress → match → shm publish |

---

## Architecture

```
Order Input (C++ or Python via pybind11)
    │
    ▼
Sequencer ─── assigns seq num, writes BBO to seqlock array
    │
    ▼
Matching Engine
    ├── per-symbol Order Book (flat_map levels, intrusive FIFO lists)
    └── generates TradeEvent / ExecutionReport
    │
    ▼
Market Data Publisher
    ├── IEX DEEP 1.0 encoder (binary wire format)
    └── writes to SPSC rings in shared memory
    │
    ▼
Shared Memory (shm_open, macOS /tmp/shm)
    ├── BBO seqlock array    ← Python MarketDataReader reads here
    ├── Trade SPSC ring      ← Python MarketDataReader drains here
    └── Quote SPSC ring

    ▲
    └── pybind11 module (iex_engine)
        Engine.submit_order() / cancel_order() / get_bbo()
        MarketDataReader.drain_trades() / get_bbo()
```

---

## Repository Layout

```
engine/         — matching core (types, pool, book, engine)
ipc/            — lock-free primitives (seqlock, spsc, shm, sequencer)
market_data/    — IEX encoder and publisher
python/         — pybind11 module (iex_engine.cpp)
notebooks/      — Jupyter notebooks driving the engine
tools/          — CLI order injector
tests/
  unit/         — fast, single-threaded
  integration/  — full pipeline, multi-threaded
  bench/        — Google Benchmark suite
docs/
  iex-deep-1.0.pdf
```

---

## Building (MacOS)

### Prerequisites

```bash
brew install llvm cmake ninja
brew install vcpkg

# Add to ~/.zshrc or ~/.bashrc:
export PATH="$(brew --prefix llvm)/bin:$PATH"
export CC="$(brew --prefix llvm)/bin/clang"
export CXX="$(brew --prefix llvm)/bin/clang++"
```

Verify: `clang++ --version` should show 17.0+.

### CMake Presets

```bash
cmake --preset debug   && cmake --build --preset debug   # ASan + UBSan
cmake --preset release && cmake --build --preset release # optimised
cmake --preset bench   && cmake --build --preset bench   # + frame pointers
cmake --preset tsan    && cmake --build --preset tsan    # ThreadSanitizer
```

### Running Tests

```bash
ctest --preset debug   # runs unit + integration under ASan
ctest --preset tsan    # runs concurrency tests under TSan
```

### Running Benchmarks

```bash
cmake --preset bench && cmake --build --preset bench
./build/bench/engine_benchmarks \
  --benchmark_repetitions=50 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=bench_results.json \
  --benchmark_out_format=json
```

### Python Binding

```bash
cmake --preset release && cmake --build --preset release --target iex_engine
pip install jupyter matplotlib pandas

# Verify the binding works:
python -c "import iex_engine; e = iex_engine.Engine(); print('ok')"

# Open notebooks:
jupyter lab notebooks/
```

---

## Using the Engine from Python

```python
import iex_engine

engine = iex_engine.Engine(max_orders=100_000)

# Submit orders
buy_id  = engine.submit_order("AAPL", "buy",  150.00, 100, "limit")
sell_id = engine.submit_order("AAPL", "sell", 150.00,  60, "limit")

# Drain trades that resulted from matching
trades = engine.drain_trades()
for t in trades:
    print(f"Trade: {t.quantity} @ ${t.price:.4f}")

# Query best bid/offer
bbo = engine.get_bbo("AAPL")
print(f"BBO: {bbo.bid_size}@{bbo.bid_price:.4f} / {bbo.ask_size}@{bbo.ask_price:.4f}")

# Cancel remaining
engine.cancel_order(buy_id)
```

---

## IEX DEEP Conformance

| Hex | Type |
|---|---|
| `0x54` | Trade Report |
| `0x51` | Quote Update |
| `0x41` | Add Order |
| `0x44` | Delete Order |
| `0x45` | Order Executed |
| `0x58` | Trading Status |

All messages: 2-byte LE length, 1-byte type, 8-byte POSIX ns timestamp, 8-byte seq num.
Sequence gap detectable: `seq == prev + 1` invariant.

---

## Key Design Decisions

See `DESIGN.md` for the full tradeoff log.

| Decision | Choice | Rationale |
|---|---|---|
| Platform | macOS only | Simplicity; ARM64 TSan finds real races |
| C++ standard | C++23 | `std::expected`, `std::flat_map`, deducing `this` |
| Price representation | `int64_t` fixed-point (4dp) | Deterministic, no FP rounding |
| Hot-path memory | Pre-allocated pool only | Zero heap allocation after startup |
| Synchronization | Lock-free SPSC + seqlock | No blocking primitives on hot path |
| Error handling | `std::expected<T, E>` | No exceptions on hot path |
| Python binding | pybind11 | Clean C++23 interop, zero-copy where possible |

