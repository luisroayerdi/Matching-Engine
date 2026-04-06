// Copyright (c) 2026 IEX Matching Engine Project

#include "engine/order_book.hpp"

namespace iex {

// ---------------------------------------------------------------------------
// Internal helpers — intrusive list surgery on a PriceLevel
// ---------------------------------------------------------------------------

// Append o to the tail of the level (FIFO: new arrivals wait longest).
static void level_push_back(PriceLevel& level, Order* o) noexcept {
    o->next = nullptr;
    o->prev = level.tail;
    if (level.tail != nullptr) level.tail->next = o;
    else                       level.head = o;   // first order at this level
    level.tail   = o;
    level.total += o->quantity;
}

// Splice o out of the level. O(1): given prev/next pointers no scan needed.
static void level_remove(PriceLevel& level, Order* o) noexcept {
    if (o->prev != nullptr) o->prev->next = o->next;
    else                    level.head    = o->next;  // o was the head

    if (o->next != nullptr) o->next->prev = o->prev;
    else                    level.tail    = o->prev;  // o was the tail

    level.total -= o->quantity;
    o->next = o->prev = nullptr;
}

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------

void OrderBook::add(Order* o) noexcept {
    auto& side = (o->side == Side::kBuy) ? bids_ : asks_;
    // operator[] default-constructs an empty PriceLevel if the price is new.
    // flat_map insert is O(n) for a new key (sorted-vector shift), but new
    // price levels are rare — see DESIGN.md §4 for the tradeoff analysis.
    level_push_back(side[o->price], o);
}

void OrderBook::cancel(Order* o) noexcept {
    auto& side = (o->side == Side::kBuy) ? bids_ : asks_;
    auto  it   = side.find(o->price);
    // Precondition: the order must be in the book. If `it == end()`, the
    // caller has a logic error (e.g., cancelling a fully-filled order).
    // We guard rather than UB, but this path should never be reached in
    // correct usage.
    if (it == side.end()) return;

    PriceLevel& level = it->second;
    level_remove(level, o);

    // Erase the price level if it is now empty; flat_map erase is O(n) due
    // to the sorted-vector shift, but empty-level erasure is rare.
    if (level.head == nullptr) {
        side.erase(it);
    }
}

std::expected<void, EngineError> OrderBook::reduce(
    Order* o, Quantity delta) noexcept
{
    // Reject a reduction that would zero or underflow the remaining quantity.
    // Callers must cancel the order instead of reducing to zero.
    if (delta >= o->quantity) {
        return std::unexpected(EngineError::kReduceTooLarge);
    }

    auto& side = (o->side == Side::kBuy) ? bids_ : asks_;
    auto  it   = side.find(o->price);
    if (it == side.end()) return std::unexpected(EngineError::kOrderNotFound);

    it->second.total -= delta;
    o->quantity      -= delta;
    return {};
}

Order* OrderBook::bid_head(Price p) const noexcept {
    auto it = bids_.find(p);
    return it == bids_.end() ? nullptr : it->second.head;
}
Quantity OrderBook::bid_total(Price p) const noexcept {
    auto it = bids_.find(p);
    return it == bids_.end() ? Quantity{0} : it->second.total;
}
Order* OrderBook::ask_head(Price p) const noexcept {
    auto it = asks_.find(p);
    return it == asks_.end() ? nullptr : it->second.head;
}
Quantity OrderBook::ask_total(Price p) const noexcept {
    auto it = asks_.find(p);
    return it == asks_.end() ? Quantity{0} : it->second.total;
}

Price OrderBook::best_bid() const noexcept {
    // bids_ is sorted ascending; the best (highest) bid is at the back.
    return bids_.empty() ? Price{0} : bids_.rbegin()->first;
}

Price OrderBook::best_ask() const noexcept {
    // asks_ is sorted ascending; the best (lowest) ask is at the front.
    return asks_.empty() ? Price{0} : asks_.begin()->first;
}

} // namespace iex
