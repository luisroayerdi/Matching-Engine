#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Domain types. Every type in the hot path is defined here.
// Rules:
//   - Fixed-width integers only. No `double` or `float` anywhere.
//   - Use the alias (Price, Quantity, …), not the raw int type, so the
//     type system carries intent even where `auto` would obscure it.
//   - Every type and enum gets a static_assert on size and, where relevant,
//     alignment. These fire at compile time — no runtime cost, no silent bloat.

#include <cstdint>
#include <type_traits>

namespace iex {

// ---------------------------------------------------------------------------
// Scalar aliases
// ---------------------------------------------------------------------------

// Price — fixed-point, 4 implicit decimal places.
// $1.00 = 10'000. $0.0001 = 1. Max representable: INT64_MAX/10000 ≈ $922 trillion.
// int64_t (not uint64_t) because crossed-order price arithmetic can produce
// negative intermediates (e.g., spread = bid − ask before inversion check).
using Price = int64_t;

// Quantity — share count. uint32_t gives 0–4,294,967,295 shares per order.
// IEX DEEP encodes size as a 4-byte unsigned integer; using uint32_t keeps
// the engine representation wire-compatible with no cast at encode time.
using Quantity = uint32_t;

// OrderId — engine-assigned, monotonically increasing per session.
// uint64_t: at 10M orders/sec a 32-bit counter wraps in ~429 seconds.
// IEX DEEP uses an 8-byte order reference number; uint64_t is wire-compatible.
using OrderId = uint64_t;

// SymbolId — index into the engine's symbol table. uint16_t caps the table
// at 65,535 symbols, which covers every listed US equity with room to spare.
// Kept small so the BBO seqlock array (indexed by SymbolId) fits in L2 cache.
using SymbolId = uint16_t;

// SeqNum — IEX sequence number, 1-based, never wraps in a session.
// uint64_t matches the IEX DEEP wire format (8-byte sequence number field).
using SeqNum = uint64_t;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

// Side — stored in the Order struct; uint8_t keeps Order at one cache line.
// ASCII values 'B'/'S' make log output readable without a lookup table.
enum class Side : uint8_t {
    kBuy  = 'B',
    kSell = 'S',
};

// OrderType — determines matching semantics:
//   kLimit  — rests in the book if not immediately fillable.
//   kMarket — fills against the best available price; never rests.
//   kIOC    — immediate-or-cancel: fills what it can, cancels the remainder.
// ASCII values chosen for the same readability reason as Side.
enum class OrderType : uint8_t {
    kLimit  = 'L',
    kMarket = 'M',
    kIOC    = 'I',
};

// TradingStatus — per-symbol state machine driven by the sequencer.
//   kHalted    — no new orders accepted; existing orders frozen.
//   kQuoteOnly — accepts limit orders but does not execute matches.
//   kTrading   — normal continuous trading; full matching active.
enum class TradingStatus : uint8_t {
    kHalted    = 'H',
    kQuoteOnly = 'Q',
    kTrading   = 'T',
};

// ---------------------------------------------------------------------------
// Composite types
// ---------------------------------------------------------------------------

// BestBidOffer — the data payload published into the seqlock array.
// 32 bytes = half a cache line. The seqlock sequence number (8 bytes) plus
// this struct fits in a single 64-byte cache line, so BBO reads are one fetch.
// alignas(64) because the seqlock array element (seq + BBO) must not straddle
// two cache lines — a torn read across a line boundary can observe seq_before
// from one line and stale BBO data from a prior eviction of the other.
struct alignas(64) BestBidOffer {
    Price    bid_price{0};   // 8 bytes — 0 if no bids exist
    Quantity bid_size{0};    // 4 bytes — 0 if no bids exist
    Price    ask_price{0};   // 8 bytes — 0 if no asks exist
    Quantity ask_size{0};    // 4 bytes — 0 if no asks exist
    SymbolId symbol_id{0};   // 2 bytes
    // 6 bytes implicit padding to reach 32 bytes; laid out explicitly below
    // to make the padding visible and prevent surprises if the struct grows.
    uint8_t  _pad[6]{};
};

// TradeEvent — emitted by the matching engine for every execution.
// Consumed by the market data publisher (Phase 3) and returned to callers
// via MatchingEngine::drain_trades(). Plain data, no ownership semantics.
// 32 bytes: fits two per cache line, reducing publish loop traffic.
struct TradeEvent {
    // The incoming order that triggered the match.
    OrderId  aggressive_id{0};  // 8 bytes
    // The resting order that was (partially or fully) filled.
    OrderId  passive_id{0};     // 8 bytes
    // Fill price = the passive order's limit price (price-time priority rule).
    Price    price{0};          // 8 bytes
    Quantity quantity{0};       // 4 bytes — shares filled in this event
    SymbolId symbol_id{0};      // 2 bytes
    uint8_t  _pad[2]{};         // explicit padding to 32 bytes
};

// ---------------------------------------------------------------------------
// Compile-time size and alignment assertions
// ---------------------------------------------------------------------------

// Scalar aliases: confirm the underlying width matches what IEX DEEP expects.
static_assert(sizeof(Price)    == 8, "Price must be 8 bytes (IEX DEEP: 8-byte price field)");
static_assert(sizeof(Quantity) == 4, "Quantity must be 4 bytes (IEX DEEP: 4-byte size field)");
static_assert(sizeof(OrderId)  == 8, "OrderId must be 8 bytes (IEX DEEP: 8-byte order ref)");
static_assert(sizeof(SymbolId) == 2, "SymbolId must be 2 bytes (65535-symbol table index)");
static_assert(sizeof(SeqNum)   == 8, "SeqNum must be 8 bytes (IEX DEEP: 8-byte seq num)");

// Enums: all must be uint8_t to keep Order at one cache line.
static_assert(sizeof(Side)          == 1, "Side must be 1 byte");
static_assert(sizeof(OrderType)     == 1, "OrderType must be 1 byte");
static_assert(sizeof(TradingStatus) == 1, "TradingStatus must be 1 byte");

// BestBidOffer: 32 bytes of data, 64-byte alignment.
static_assert(sizeof(BestBidOffer)  == 64, "BestBidOffer must be exactly 64 bytes");
static_assert(alignof(BestBidOffer) == 64, "BestBidOffer must be 64-byte aligned");

// Trivially copyable check: the seqlock memcpys the BBO payload; if this fires
// it means a constructor/destructor was added that breaks the seqlock protocol.
static_assert(std::is_trivially_copyable_v<BestBidOffer>,
    "BestBidOffer must be trivially copyable for safe seqlock memcpy");

// TradeEvent: 32 bytes, trivially copyable for safe ring-buffer memcpy.
static_assert(sizeof(TradeEvent)  == 32, "TradeEvent must be 32 bytes");
static_assert(std::is_trivially_copyable_v<TradeEvent>,
    "TradeEvent must be trivially copyable for safe ring-buffer copy");

} // namespace iex
