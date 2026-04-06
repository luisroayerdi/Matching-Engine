// Copyright (c) 2026 IEX Matching Engine Project

#include "engine/matching_engine.hpp"

#include <algorithm>  // std::min
#include <cassert>

namespace iex {

MatchingEngine::MatchingEngine(std::size_t pool_capacity)
    : pool_(pool_capacity)
{
    // Pre-reserve trade event buffer so push_back never allocates on the
    // hot path. kMaxTradesPerSubmit is a hard upper bound per submit() call.
    pending_trades_.reserve(kMaxTradesPerSubmit);
}

void MatchingEngine::register_symbol(SymbolId symbol) {
    // operator[] default-constructs an empty OrderBook if not present.
    // Called only during initialisation — flat_map insert cost is acceptable.
    books_[symbol];
}

std::expected<OrderId, EngineError> MatchingEngine::submit(
    SymbolId  symbol,
    Side      side,
    OrderType type,
    Price     price,
    Quantity  qty) noexcept
{
    // --- Validate inputs ---
    if (qty == 0) {
        return std::unexpected(EngineError::kInvalidQuantity);
    }
    // Market orders have no price limit. Limit and IOC orders do: IOC is a
    // limit order that does not rest, so its price is equally meaningful.
    if (type != OrderType::kMarket && price <= 0) {
        return std::unexpected(EngineError::kInvalidPrice);
    }

    auto book_it = books_.find(symbol);
    if (book_it == books_.end()) {
        return std::unexpected(EngineError::kUnknownSymbol);
    }
    OrderBook& book = book_it->second;

    // --- Allocate Order ---
    auto alloc_result = pool_.alloc();
    if (!alloc_result.has_value()) {
        return std::unexpected(alloc_result.error());
    }
    Order* o = *alloc_result;

    o->price     = price;
    o->quantity  = qty;
    o->symbol_id = symbol;
    o->side      = side;
    o->type      = type;

    OrderId id = o->id;  // captured before potential free

    // --- Clear trade buffer for this submission ---
    pending_trades_.clear();

    // --- Match against the opposite side ---
    match_order(book, o);

    // --- Disposition of remainder ---
    if (o->quantity > 0) {
        if (type == OrderType::kLimit) {
            // Resting limit order: add to the book.
            book.add(o);
        } else {
            // Market or IOC: cancel unfilled remainder.
            pool_.free(o);
        }
    } else {
        // Fully filled: free the slot.
        pool_.free(o);
    }

    return id;
}

std::expected<void, EngineError> MatchingEngine::cancel(OrderId id) noexcept {
    Order* o = pool_.get(id);
    if (o == nullptr) {
        return std::unexpected(EngineError::kOrderNotFound);
    }

    auto book_it = books_.find(o->symbol_id);
    if (book_it == books_.end()) {
        // Should never happen: an Order in the pool always has a valid symbol.
        return std::unexpected(EngineError::kUnknownSymbol);
    }

    book_it->second.cancel(o);
    pool_.free(o);
    return {};
}

std::span<const TradeEvent> MatchingEngine::drain_trades() noexcept {
    return std::span<const TradeEvent>{pending_trades_};
}

std::optional<BestBidOffer> MatchingEngine::get_bbo(
    SymbolId symbol) const noexcept
{
    auto it = books_.find(symbol);
    if (it == books_.end()) return std::nullopt;

    const OrderBook& book = it->second;
    BestBidOffer bbo{};
    bbo.symbol_id = symbol;
    bbo.bid_price = book.best_bid();
    bbo.ask_price = book.best_ask();

    // Walk the best level for bid size and ask size.
    bbo.bid_size = book.bid_total(bbo.bid_price);
    bbo.ask_size = book.ask_total(bbo.ask_price);

    return bbo;
}

// ---------------------------------------------------------------------------
// Private: matching loop
// ---------------------------------------------------------------------------

void MatchingEngine::match_order(OrderBook& book, Order* aggressor) noexcept {
    // Select the passive side based on the aggressor's direction.
    // Buy aggressor crosses against asks (ascending); best = begin() (lowest).
    // Sell aggressor crosses against bids (ascending); best = rbegin() (highest).
    bool is_buy = (aggressor->side == Side::kBuy);
    auto& passive_side = is_buy ? book.asks_ : book.bids_;

    while (aggressor->quantity > 0 && !passive_side.empty()) {
        // Get the best price level on the passive side.
        auto best_it = is_buy ? passive_side.begin()
                              : std::prev(passive_side.end());
        Price passive_price = best_it->first;

        // Price-time priority crossing check.
        // Market orders cross at any price. Limit and IOC orders only cross
        // if the spread has closed at the aggressor's price — IOC is a limit
        // order that does not rest, so its price limit is equally binding.
        if (aggressor->type != OrderType::kMarket) {
            bool crosses = is_buy ? (aggressor->price >= passive_price)
                                  : (aggressor->price <= passive_price);
            if (!crosses) break;
        }

        // Walk the FIFO queue at this price level, filling oldest first.
        PriceLevel& level = best_it->second;
        while (aggressor->quantity > 0 && level.head != nullptr) {
            Order*   passive  = level.head;
            Quantity fill_qty = std::min(aggressor->quantity, passive->quantity);

            pending_trades_.push_back(TradeEvent{
                .aggressive_id = aggressor->id,
                .passive_id    = passive->id,
                .price         = passive_price,
                .quantity      = fill_qty,
                .symbol_id     = aggressor->symbol_id,
            });

            aggressor->quantity -= fill_qty;
            passive->quantity   -= fill_qty;
            level.total         -= fill_qty;

            if (passive->quantity == 0) {
                // Fully filled: splice out and return to pool.
                level.head = passive->next;
                if (level.head != nullptr) level.head->prev = nullptr;
                else                       level.tail = nullptr;

                pool_.free(passive);
            }
        }

        // If the level is now empty, erase it. After erase, best_it is
        // invalidated; the next loop iteration gets a fresh iterator.
        if (level.head == nullptr) {
            passive_side.erase(best_it);
        }
    }
}

} // namespace iex
