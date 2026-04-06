#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Per-symbol order book: price-time priority.
//
// Structure:
//   bids_ — std::flat_map<Price, PriceLevel>, sorted ascending.
//            Best bid = rbegin() (highest price).
//   asks_ — std::flat_map<Price, PriceLevel>, sorted ascending.
//            Best ask = begin() (lowest price).
//
// PriceLevel holds an intrusive FIFO doubly-linked list of pool-allocated
// Orders. Appending/removing is O(1) pointer surgery with no heap traffic.
// A new PriceLevel is created when the first order arrives at a price; it is
// erased from the flat_map when its last order is removed. New-level creation
// is O(n) due to flat_map's sorted-vector insert; this is accepted because new
// price levels are rare relative to order arrivals at existing levels.

#include "engine/errors.hpp"
#include "engine/order_pool.hpp"

#include <expected>
#include <flat_map>

namespace iex {

// ---------------------------------------------------------------------------
// PriceLevel — intrusive FIFO queue of Orders at a single price
// ---------------------------------------------------------------------------

struct PriceLevel {
    Order*   head{nullptr};   // oldest order at this level — filled first (FIFO)
    Order*   tail{nullptr};   // newest order — new arrivals appended here
    Quantity total{0};        // sum of remaining quantities across all orders
};

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------

class OrderBook {
    // MatchingEngine accesses bids_/asks_ directly for the matching loop.
    // No other class should reach into the book's internals.
    friend class MatchingEngine;

public:
    OrderBook()  = default;
    ~OrderBook() = default;

    // Non-copyable: intrusive pointers inside Orders would dangle.
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Movable: the flat_maps and pointers transfer correctly.
    OrderBook(OrderBook&&)            = default;
    OrderBook& operator=(OrderBook&&) = default;

    // Insert `o` into the appropriate side at o->price.
    // Creates a new PriceLevel if none exists at that price.
    // The Order must be live (allocated from pool, not yet freed).
    void add(Order* o) noexcept;

    // Splice `o` out of its price level.
    // Erases the PriceLevel from the flat_map if it becomes empty.
    // Does NOT free `o` back to the pool — the caller is responsible.
    void cancel(Order* o) noexcept;

    // Reduce `o`'s remaining quantity by `delta`.
    // Returns kReduceTooLarge if delta >= o->quantity (would zero or underflow).
    // On success, also decrements the PriceLevel's total.
    [[nodiscard]] std::expected<void, EngineError> reduce(
        Order* o, Quantity delta) noexcept;

    // Best bid price. Returns 0 if there are no bids.
    [[nodiscard]] Price best_bid() const noexcept;

    // Best ask price. Returns 0 if there are no asks.
    [[nodiscard]] Price best_ask() const noexcept;

    [[nodiscard]] bool bids_empty() const noexcept { return bids_.empty(); }
    [[nodiscard]] bool asks_empty() const noexcept { return asks_.empty(); }

    // Level inspection — used by tests and get_bbo(). Returns nullptr / 0 if
    // no orders exist at that price.
    [[nodiscard]] Order*    bid_head (Price p) const noexcept;
    [[nodiscard]] Quantity  bid_total(Price p) const noexcept;
    [[nodiscard]] Order*    ask_head (Price p) const noexcept;
    [[nodiscard]] Quantity  ask_total(Price p) const noexcept;

private:
    // Both maps are sorted ascending by Price.
    // bid side: best = rbegin() (max price)
    // ask side: best = begin()  (min price)
    std::flat_map<Price, PriceLevel> bids_;
    std::flat_map<Price, PriceLevel> asks_;
};

} // namespace iex
