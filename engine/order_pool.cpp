// Copyright (c) 2026 IEX Matching Engine Project

#include "engine/order_pool.hpp"

#include <cassert>
#include <cstring>
#include <utility>   // std::exchange

namespace iex {

// ---------------------------------------------------------------------------
// Helpers — OrderId encoding
// ---------------------------------------------------------------------------

// OrderId layout: high 32 bits = generation, low 32 bits = slot index.
// Generation starts at 1 so the smallest valid OrderId is (1 << 32) — never 0.
// OrderId 0 is the sentinel for "no order".

static OrderId make_id(uint32_t slot, uint32_t gen) noexcept {
    return (static_cast<OrderId>(gen) << 32) | static_cast<OrderId>(slot);
}

static uint32_t slot_of(OrderId id) noexcept {
    return static_cast<uint32_t>(id);          // low 32 bits
}

static uint32_t gen_of(OrderId id) noexcept {
    return static_cast<uint32_t>(id >> 32);    // high 32 bits
}

// ---------------------------------------------------------------------------
// OrderPool
// ---------------------------------------------------------------------------

OrderPool::OrderPool(std::size_t capacity) : capacity_(capacity) {
    // Single allocation: Order array immediately followed by the generation
    // array. One huge-page call rather than two separate allocations minimises
    // TLB entries on Linux and simplifies cleanup.
    std::size_t order_bytes = capacity * sizeof(Order);
    std::size_t gen_bytes   = capacity * sizeof(uint32_t);
    alloc_bytes_            = order_bytes + gen_bytes;

    void* mem   = platform::alloc_huge(alloc_bytes_);
    storage_    = static_cast<Order*>(mem);
    generation_ = reinterpret_cast<uint32_t*>(
                      static_cast<uint8_t*>(mem) + order_bytes);

    // Zero all Order fields. mmap guarantees zero pages on both platforms, but
    // explicit memset makes the intent clear and works even if the allocator
    // ever changes to reuse pages.
    std::memset(storage_, 0, order_bytes);

    // Generation starts at 1. This ensures the first OrderId issued for any
    // slot is non-zero (= valid sentinel), and that generation 0 is reserved
    // for "slot never allocated" detection if needed in the future.
    for (std::size_t i = 0; i < capacity_; ++i) {
        generation_[i] = 1;
    }

    // Build the intrusive free list: each slot's next pointer chains to the
    // following slot. Last slot's next is nullptr (end of list).
    for (std::size_t i = 0; i + 1 < capacity_; ++i) {
        storage_[i].next = &storage_[i + 1];
    }
    storage_[capacity_ - 1].next = nullptr;

    free_head_  = &storage_[0];
    free_count_ = capacity_;
}

OrderPool::~OrderPool() {
    if (storage_ != nullptr) {
        platform::free_huge(storage_, alloc_bytes_);
    }
}

OrderPool::OrderPool(OrderPool&& other) noexcept
    : storage_    (std::exchange(other.storage_,     nullptr))
    , generation_ (std::exchange(other.generation_,  nullptr))
    , free_head_  (std::exchange(other.free_head_,   nullptr))
    , capacity_   (std::exchange(other.capacity_,    0))
    , free_count_ (std::exchange(other.free_count_,  0))
    , alloc_bytes_(std::exchange(other.alloc_bytes_,  0))
{}

OrderPool& OrderPool::operator=(OrderPool&& other) noexcept {
    if (this != &other) {
        if (storage_ != nullptr) platform::free_huge(storage_, alloc_bytes_);
        storage_     = std::exchange(other.storage_,    nullptr);
        generation_  = std::exchange(other.generation_, nullptr);
        free_head_   = std::exchange(other.free_head_,  nullptr);
        capacity_    = std::exchange(other.capacity_,   0);
        free_count_  = std::exchange(other.free_count_, 0);
        alloc_bytes_ = std::exchange(other.alloc_bytes_, 0);
    }
    return *this;
}

std::expected<Order*, EngineError> OrderPool::alloc() noexcept {
    if (free_head_ == nullptr) {
        return std::unexpected(EngineError::kPoolExhausted);
    }

    Order* o   = free_head_;
    free_head_ = o->next;      // pop from free list
    --free_count_;

    // Stamp the slot with a fresh OrderId using its current generation.
    // The generation was already incremented when this slot was last freed
    // (or is 1 for a never-used slot), so the new id is distinct from any
    // previously issued id for this slot.
    uint32_t slot = static_cast<uint32_t>(o - storage_);
    o->id         = make_id(slot, generation_[slot]);
    o->next       = nullptr;
    o->prev       = nullptr;

    return o;
}

void OrderPool::free(Order* o) noexcept {
    uint32_t slot = static_cast<uint32_t>(o - storage_);

    // Increment generation: any outstanding OrderId encoding the old generation
    // for this slot will now be rejected by get(). This is the O(1) stale-
    // reference detection that avoids the need for a hash map.
    generation_[slot]++;

    o->id   = 0;          // mark as free; makes use-after-free easier to spot
    o->prev = nullptr;    // not used in free list
    o->next = free_head_; // push onto free list
    free_head_ = o;
    ++free_count_;
}

Order* OrderPool::get(OrderId id) const noexcept {
    uint32_t slot = slot_of(id);
    uint32_t gen  = gen_of(id);

    if (slot >= capacity_) return nullptr;

    // A mismatch means the slot has been freed (and possibly reallocated) since
    // this id was issued. The caller attempted to use a stale reference.
    if (generation_[slot] != gen) return nullptr;

    return storage_ + slot;
}

} // namespace iex
