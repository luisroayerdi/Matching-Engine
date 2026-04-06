#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Fixed-size slab allocator for Order objects.
//
// Design:
//   - Pre-allocates N × 64-byte Order slots via platform::alloc_huge at
//     construction time. Zero heap traffic after startup.
//   - Free list is intrusive: freed Orders reuse their `next` pointer to
//     chain free slots. Safe because a slot is either pool-owned (free list)
//     or book-owned (price level), never both simultaneously.
//   - OrderId encoding: high 32 bits = generation, low 32 bits = slot index.
//     Incrementing generation on free makes stale references detectable in O(1)
//     via get(OrderId) without a hash map.

#include "engine/errors.hpp"
#include "engine/types.hpp"
#include "platform/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <type_traits>

namespace iex {

// ---------------------------------------------------------------------------
// Order — one cache line, intrusive doubly-linked list
// ---------------------------------------------------------------------------

// Hot-path Order: exactly one cache line (64 bytes).
// Pointers first to avoid padding between two 8-byte pointer fields.
// The intrusive list (next/prev) serves double duty:
//   - When live (book-owned): points to neighbouring orders in price level.
//   - When free (pool-owned): next points to the next free slot; prev unused.
struct alignas(64) Order {
    Order*    next{nullptr};              //  8 — intrusive list or free-list chain
    Order*    prev{nullptr};              //  8 — intrusive list (nullptr if head or free)
    Price     price{0};                   //  8 — fixed-point, 4dp
    OrderId   id{0};                      //  8 — (generation << 32) | slot_index
    Quantity  quantity{0};                //  4 — remaining; decremented on partial fill
    SymbolId  symbol_id{0};               //  2
    Side      side{Side::kBuy};           //  1
    OrderType type{OrderType::kLimit};    //  1
    // 40 bytes above; 24 bytes explicit padding to fill cache line.
    // Reserved for future fields (e.g., timestamp, display qty, iceberg peak).
    uint8_t   _pad[24]{};
};

static_assert(sizeof(Order)  == 64, "Order must be exactly one cache line (64 bytes)");
static_assert(alignof(Order) == 64, "Order must be 64-byte aligned");
// Trivially copyable: the pool uses memset to zero-init slots; a non-trivial
// destructor would make that unsafe.
static_assert(std::is_trivially_copyable_v<Order>,
    "Order must be trivially copyable — do not add non-trivial members");

// ---------------------------------------------------------------------------
// OrderPool
// ---------------------------------------------------------------------------

class OrderPool {
public:
    // Allocate `capacity` Order slots backed by platform::alloc_huge.
    // Throws std::bad_alloc (via alloc_huge) if memory cannot be obtained.
    // Call only during engine initialisation, before the hot path starts.
    explicit OrderPool(std::size_t capacity);

    ~OrderPool();

    // Non-copyable: owns raw huge-page memory.
    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    // Movable: transfer ownership of the allocation.
    OrderPool(OrderPool&&) noexcept;
    OrderPool& operator=(OrderPool&&) noexcept;

    // Pop a slot from the free list and stamp it with a fresh OrderId.
    // The returned Order* has: id set, next/prev cleared, all other fields zeroed.
    // Caller must fill price, quantity, symbol_id, side, type before inserting
    // into a book.
    // Returns kPoolExhausted if no free slots remain.
    [[nodiscard]] std::expected<Order*, EngineError> alloc() noexcept;

    // Return an Order slot to the free list.
    // Increments the generation counter so any outstanding OrderId for this
    // slot becomes stale and get() will return nullptr for it.
    // Behaviour is undefined if `o` was not returned by this pool's alloc(),
    // or if `o` has already been freed.
    void free(Order* o) noexcept;

    // Look up an Order by its full OrderId (including generation).
    // Returns nullptr if:
    //   - The slot index in `id` is out of range.
    //   - The generation in `id` does not match the current generation for
    //     that slot (i.e., the order was freed after this id was issued).
    [[nodiscard]] Order* get(OrderId id) const noexcept;

    [[nodiscard]] std::size_t capacity()   const noexcept { return capacity_; }
    [[nodiscard]] std::size_t free_count() const noexcept { return free_count_; }

private:
    Order*     storage_{nullptr};    // huge-page-backed Order array
    uint32_t*  generation_{nullptr}; // parallel array: generation_[slot] for each slot
    Order*     free_head_{nullptr};  // top of intrusive free-list stack
    std::size_t capacity_{0};
    std::size_t free_count_{0};

    // Total bytes of the single huge-page allocation (Orders + generation array).
    std::size_t alloc_bytes_{0};
};

} // namespace iex
