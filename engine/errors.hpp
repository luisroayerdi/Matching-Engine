#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Engine error codes. All fallible hot-path operations return
// std::expected<T, EngineError>. This enum is the E in every such return type.

#include <cstdint>
#include <string_view>

namespace iex {

enum class EngineError : uint8_t {
    // Order submission errors
    kPoolExhausted    = 1,  // Order pool has no free slots — pre-allocated limit hit.
    kUnknownSymbol    = 2,  // SymbolId not registered in the symbol table.
    kInvalidPrice     = 3,  // Price ≤ 0 for a limit order, or overflow of fixed-point range.
    kInvalidQuantity  = 4,  // Quantity == 0.
    kDuplicateOrderId = 5,  // OrderId already exists in the active-order map.

    // Order management errors
    kOrderNotFound    = 6,  // Cancel/reduce refers to an OrderId not in the active map.
    kReduceTooLarge   = 7,  // Reduce-quantity ≥ remaining quantity (would zero or underflow).

    // IPC / sequencer errors
    kQueueFull        = 8,  // SPSC ring is full — oldest entry will be overwritten (lossy).
    kShmCreateFailed  = 9,  // shm_open() or mmap() returned an error.
    kShmSizeMismatch  = 10, // Existing shm segment has wrong size (stale from prior run).
};

// Returns a human-readable string for an EngineError value.
// string_view is non-owning and points into static storage — zero allocation.
[[nodiscard]] constexpr std::string_view to_string(EngineError e) noexcept {
    switch (e) {
        case EngineError::kPoolExhausted:    return "pool exhausted";
        case EngineError::kUnknownSymbol:    return "unknown symbol";
        case EngineError::kInvalidPrice:     return "invalid price";
        case EngineError::kInvalidQuantity:  return "invalid quantity";
        case EngineError::kDuplicateOrderId: return "duplicate order id";
        case EngineError::kOrderNotFound:    return "order not found";
        case EngineError::kReduceTooLarge:   return "reduce quantity too large";
        case EngineError::kQueueFull:        return "ipc queue full";
        case EngineError::kShmCreateFailed:  return "shared memory create failed";
        case EngineError::kShmSizeMismatch:  return "shared memory size mismatch";
    }
    return "unknown error";  // unreachable, but satisfies -Wreturn-type
}

} // namespace iex
