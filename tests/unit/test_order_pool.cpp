// Copyright (c) 2026 IEX Matching Engine Project
// OrderPool unit tests.

#include "engine/order_pool.hpp"

#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

using namespace iex;

// ---------------------------------------------------------------------------
// Fixture: small pool to exercise boundary conditions cheaply
// ---------------------------------------------------------------------------

class PoolTest : public ::testing::Test {
protected:
    static constexpr std::size_t kCap = 8;
    OrderPool pool{kCap};
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(PoolTest, AllocReturnsValidPointer) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(*r, nullptr);
}

TEST_F(PoolTest, AllocedOrderHasNonZeroId) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    EXPECT_NE((*r)->id, OrderId{0});
}

TEST_F(PoolTest, AllocedOrderHasNullLinks) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    Order* o = *r;
    EXPECT_EQ(o->next, nullptr);
    EXPECT_EQ(o->prev, nullptr);
}

TEST_F(PoolTest, FreeCountDecrementsOnAlloc) {
    EXPECT_EQ(pool.free_count(), kCap);
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(pool.free_count(), kCap - 1);
}

TEST_F(PoolTest, FreeCountIncrementsOnFree) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    pool.free(*r);
    EXPECT_EQ(pool.free_count(), kCap);
}

// ---------------------------------------------------------------------------
// get() — stale-reference detection via generation counter
// ---------------------------------------------------------------------------

TEST_F(PoolTest, GetByIdFindsLiveOrder) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    OrderId id = (*r)->id;
    EXPECT_EQ(pool.get(id), *r);
}

TEST_F(PoolTest, GetByIdReturnsNullAfterFree) {
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
    OrderId id = (*r)->id;
    pool.free(*r);
    // Generation was incremented on free — old id is stale.
    EXPECT_EQ(pool.get(id), nullptr);
}

TEST_F(PoolTest, GetByIdReturnsNullAfterReallocSameSlot) {
    // Alloc, capture id, free, alloc again (same slot with new generation).
    // The old id must no longer be valid.
    auto r1 = pool.alloc();
    ASSERT_TRUE(r1.has_value());
    OrderId stale_id = (*r1)->id;
    pool.free(*r1);

    auto r2 = pool.alloc();
    ASSERT_TRUE(r2.has_value());
    OrderId fresh_id = (*r2)->id;

    EXPECT_EQ(pool.get(stale_id), nullptr);  // old generation
    EXPECT_EQ(pool.get(fresh_id), *r2);      // new generation
}

TEST_F(PoolTest, GetByIdReturnsNullForOutOfRangeSlot) {
    // Craft an id with a slot index beyond pool capacity.
    OrderId bad_id = static_cast<OrderId>(kCap + 1000);
    EXPECT_EQ(pool.get(bad_id), nullptr);
}

// ---------------------------------------------------------------------------
// Pool exhaustion
// ---------------------------------------------------------------------------

TEST_F(PoolTest, ExhaustionReturnsError) {
    // Drain all slots.
    std::vector<Order*> held;
    held.reserve(kCap);
    for (std::size_t i = 0; i < kCap; ++i) {
        auto r = pool.alloc();
        ASSERT_TRUE(r.has_value()) << "alloc failed at i=" << i;
        held.push_back(*r);
    }
    EXPECT_EQ(pool.free_count(), 0u);

    // One more alloc must fail with kPoolExhausted.
    auto r = pool.alloc();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kPoolExhausted);
}

TEST_F(PoolTest, ExhaustionThenFreeAndReallocSucceeds) {
    std::vector<Order*> held;
    held.reserve(kCap);
    for (std::size_t i = 0; i < kCap; ++i) {
        held.push_back(*pool.alloc());
    }
    // Free one and re-alloc.
    pool.free(held.back());
    held.pop_back();
    auto r = pool.alloc();
    ASSERT_TRUE(r.has_value());
}

// ---------------------------------------------------------------------------
// Uniqueness — each alloc returns a distinct pointer and id
// ---------------------------------------------------------------------------

TEST_F(PoolTest, AllAllocedPointersAreDistinct) {
    std::unordered_set<Order*> ptrs;
    for (std::size_t i = 0; i < kCap; ++i) {
        auto r = pool.alloc();
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(ptrs.insert(*r).second) << "duplicate pointer at i=" << i;
    }
}

TEST_F(PoolTest, AllAllocedIdsAreDistinct) {
    std::unordered_set<OrderId> ids;
    std::vector<Order*> held;
    // Multi-round: alloc all, free all, alloc all again — generations differ.
    for (int round = 0; round < 2; ++round) {
        held.clear();
        for (std::size_t i = 0; i < kCap; ++i) {
            auto r = pool.alloc();
            ASSERT_TRUE(r.has_value());
            OrderId id = (*r)->id;
            EXPECT_TRUE(ids.insert(id).second) << "duplicate id round=" << round;
            held.push_back(*r);
        }
        for (Order* o : held) pool.free(o);
    }
}

// ---------------------------------------------------------------------------
// Boundary: pool with a single slot
// ---------------------------------------------------------------------------

TEST(PoolBoundary, SingleSlot) {
    OrderPool pool(1);
    EXPECT_EQ(pool.capacity(), 1u);
    EXPECT_EQ(pool.free_count(), 1u);

    auto r1 = pool.alloc();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(pool.free_count(), 0u);

    auto r2 = pool.alloc();
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), EngineError::kPoolExhausted);

    pool.free(*r1);
    EXPECT_EQ(pool.free_count(), 1u);

    auto r3 = pool.alloc();
    ASSERT_TRUE(r3.has_value());
}
