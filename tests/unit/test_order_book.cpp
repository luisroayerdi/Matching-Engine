// Copyright (c) 2026 IEX Matching Engine Project
// OrderBook unit tests.
// Uses a small OrderPool as backing store; pool management is explicit so
// the tests exercise the book in isolation from the matching engine.

#include "engine/order_book.hpp"
#include "engine/order_pool.hpp"

#include <gtest/gtest.h>

using namespace iex;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Allocate an order and fill its fields from a pool.
static Order* make_order(OrderPool& pool, Side side, Price price, Quantity qty,
                         SymbolId sym = SymbolId{1}) {
    auto r = pool.alloc();
    if (!r.has_value()) return nullptr;
    Order* o     = *r;
    o->side      = side;
    o->price     = price;
    o->quantity  = qty;
    o->symbol_id = sym;
    o->type      = OrderType::kLimit;
    return o;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class BookTest : public ::testing::Test {
protected:
    OrderPool pool{64};
    OrderBook book;
};

// ---------------------------------------------------------------------------
// Empty book
// ---------------------------------------------------------------------------

TEST_F(BookTest, EmptyBookBBOIsZero) {
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), 0);
    EXPECT_TRUE(book.bids_empty());
    EXPECT_TRUE(book.asks_empty());
}

// ---------------------------------------------------------------------------
// Add single orders
// ---------------------------------------------------------------------------

TEST_F(BookTest, AddBidSetsBestBid) {
    Order* o = make_order(pool, Side::kBuy, 100'000, 50);
    book.add(o);
    EXPECT_EQ(book.best_bid(), 100'000);
}

TEST_F(BookTest, AddAskSetsBestAsk) {
    Order* o = make_order(pool, Side::kSell, 101'000, 30);
    book.add(o);
    EXPECT_EQ(book.best_ask(), 101'000);
}

// ---------------------------------------------------------------------------
// Multiple price levels — best bid = highest, best ask = lowest
// ---------------------------------------------------------------------------

TEST_F(BookTest, BestBidIsHighestPrice) {
    Order* o1 = make_order(pool, Side::kBuy, 100'000, 10);
    Order* o2 = make_order(pool, Side::kBuy, 102'000, 20);
    Order* o3 = make_order(pool, Side::kBuy,  98'000, 30);
    book.add(o1); book.add(o2); book.add(o3);
    EXPECT_EQ(book.best_bid(), 102'000);
}

TEST_F(BookTest, BestAskIsLowestPrice) {
    Order* o1 = make_order(pool, Side::kSell, 105'000, 10);
    Order* o2 = make_order(pool, Side::kSell, 103'000, 20);
    Order* o3 = make_order(pool, Side::kSell, 107'000, 30);
    book.add(o1); book.add(o2); book.add(o3);
    EXPECT_EQ(book.best_ask(), 103'000);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST_F(BookTest, CancelRemovesOrder) {
    Order* o = make_order(pool, Side::kBuy, 100'000, 50);
    book.add(o);
    EXPECT_EQ(book.best_bid(), 100'000);

    book.cancel(o);
    pool.free(o);
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_TRUE(book.bids_empty());
}

TEST_F(BookTest, CancelOneLevelLeavesOthers) {
    Order* o1 = make_order(pool, Side::kBuy, 100'000, 10);
    Order* o2 = make_order(pool, Side::kBuy, 102'000, 20);
    book.add(o1); book.add(o2);

    // Cancel the best bid; 100'000 should remain.
    book.cancel(o2);
    pool.free(o2);
    EXPECT_EQ(book.best_bid(), 100'000);
}

TEST_F(BookTest, CancelOneOfTwoAtSameLevel) {
    // FIFO: o1 at front, o2 at back. Cancel o1; o2 remains.
    Order* o1 = make_order(pool, Side::kBuy, 100'000, 10);
    Order* o2 = make_order(pool, Side::kBuy, 100'000, 20);
    book.add(o1); book.add(o2);

    book.cancel(o1);
    pool.free(o1);

    // Level still exists at 100'000.
    EXPECT_EQ(book.best_bid(), 100'000);
    // o2 is now the head.
    EXPECT_EQ(book.bid_head(100'000),  o2);
    EXPECT_EQ(book.bid_total(100'000), Quantity{20});
}

// ---------------------------------------------------------------------------
// FIFO order preservation
// ---------------------------------------------------------------------------

TEST_F(BookTest, FifoOrderPreserved) {
    // Add three orders at the same price; verify head->next->next chain.
    Order* o1 = make_order(pool, Side::kSell, 101'000, 10);
    Order* o2 = make_order(pool, Side::kSell, 101'000, 20);
    Order* o3 = make_order(pool, Side::kSell, 101'000, 30);
    book.add(o1); book.add(o2); book.add(o3);

    Order* head = book.ask_head(101'000);
    EXPECT_EQ(head,             o1);
    EXPECT_EQ(head->next,       o2);
    EXPECT_EQ(head->next->next, o3);
    EXPECT_EQ(book.ask_total(101'000), Quantity{60});
}

// ---------------------------------------------------------------------------
// Reduce
// ---------------------------------------------------------------------------

TEST_F(BookTest, ReduceDecrementsQuantityAndLevelTotal) {
    Order* o = make_order(pool, Side::kBuy, 100'000, 50);
    book.add(o);

    auto r = book.reduce(o, 20);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(o->quantity, Quantity{30});
    EXPECT_EQ(book.bid_total(100'000), Quantity{30});
}

TEST_F(BookTest, ReduceTooLargeReturnsError) {
    Order* o = make_order(pool, Side::kBuy, 100'000, 50);
    book.add(o);

    auto r = book.reduce(o, 50);  // delta == quantity → error
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kReduceTooLarge);
}

TEST_F(BookTest, ReduceByZeroSucceeds) {
    // Reducing by 0 is a no-op, not an error.
    Order* o = make_order(pool, Side::kBuy, 100'000, 50);
    book.add(o);
    auto r = book.reduce(o, 0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(o->quantity, Quantity{50});
}

// ---------------------------------------------------------------------------
// PriceLevel total tracks multi-order sum
// ---------------------------------------------------------------------------

TEST_F(BookTest, LevelTotalIsSumOfQuantities) {
    Order* o1 = make_order(pool, Side::kBuy, 100'000, 10);
    Order* o2 = make_order(pool, Side::kBuy, 100'000, 25);
    Order* o3 = make_order(pool, Side::kBuy, 100'000, 15);
    book.add(o1); book.add(o2); book.add(o3);
    EXPECT_EQ(book.bid_total(100'000), Quantity{50});
}
