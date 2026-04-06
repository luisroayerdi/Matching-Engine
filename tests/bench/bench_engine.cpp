// Copyright (c) 2026 IEX Matching Engine Project
// Phase 1 benchmark suite — macOS algorithmic baselines.
//
// Targets (p99, macOS Apple Silicon):
//   BM_OrderInsert     < 300 ns
//   BM_MatchTwoSided   < 800 ns
//
// Run with:
//   ./build/bench/engine_benchmarks             \
//     --benchmark_repetitions=50               \
//     --benchmark_report_aggregates_only=true  \
//     --benchmark_out=bench_results.json       \
//     --benchmark_out_format=json

#include "engine/matching_engine.hpp"

#include <benchmark/benchmark.h>

using namespace iex;

// Fixed-point prices
static constexpr Price    kAskPrice  = 100'0000;  // $100.00
static constexpr Price    kBidPrice  =  99'0000;  // $99.00  (non-crossing)
static constexpr SymbolId kSym       = SymbolId{1};
static constexpr Quantity kQty       = 100;

// ---------------------------------------------------------------------------
// BM_OrderInsert — single limit order insert, no match
// ---------------------------------------------------------------------------
// Measures the cost of submit() when no crossing occurs: pool alloc + flat_map
// insert at an existing price level. The order is cancelled after each insert
// to keep the book from growing and to return the slot to the pool, but the
// cancel is excluded from the timed region.

static void BM_OrderInsert(benchmark::State& state) {
    // 500K iterations × 1 slot each; pool is large enough for all timed ops.
    MatchingEngine engine(600'000);
    engine.register_symbol(kSym);

    // Pre-warm: insert one order at kBidPrice so the price level exists and
    // subsequent inserts exercise the existing-level path (O(1) list append),
    // not the new-level path (O(n) flat_map insert). New-level creation is
    // rare and not what this benchmark measures.
    [[maybe_unused]] auto warmup =
        engine.submit(kSym, Side::kBuy, OrderType::kLimit, kBidPrice, kQty);

    for (auto _ : state) {
        auto r = engine.submit(kSym, Side::kBuy, OrderType::kLimit, kBidPrice, kQty);
        benchmark::DoNotOptimize(r);

        // Cancel outside the timed region.
        state.PauseTiming();
        if (r.has_value()) (void)engine.cancel(*r);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_OrderInsert)->Iterations(200'000)->Unit(benchmark::kNanosecond);

// ---------------------------------------------------------------------------
// BM_MatchTwoSided — aggressive order crosses a single resting order
// ---------------------------------------------------------------------------
// Measures the cost of submit() when a full match occurs: pool alloc + one
// passive-side scan + one trade event appended + passive order freed.
// The passive order is set up outside the timed region.

static void BM_MatchTwoSided(benchmark::State& state) {
    // 2 slots per iteration: one for the passive, one for the aggressive.
    MatchingEngine engine(2'000'000);
    engine.register_symbol(kSym);

    for (auto _ : state) {
        // Set up a resting sell — paused, not part of the measurement.
        state.PauseTiming();
        [[maybe_unused]] auto passive =
            engine.submit(kSym, Side::kSell, OrderType::kLimit, kAskPrice, kQty);
        state.ResumeTiming();

        // Aggressive buy crosses the resting sell.
        auto r = engine.submit(kSym, Side::kBuy, OrderType::kLimit, kAskPrice, kQty);
        benchmark::DoNotOptimize(r);

        // Drain trades to reset the buffer for the next iteration.
        state.PauseTiming();
        benchmark::DoNotOptimize(engine.drain_trades());
        state.ResumeTiming();
    }
}
BENCHMARK(BM_MatchTwoSided)->Iterations(500'000)->Unit(benchmark::kNanosecond);
