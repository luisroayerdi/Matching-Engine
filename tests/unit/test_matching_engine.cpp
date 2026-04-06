// Copyright (c) 2026 IEX Matching Engine Project
// MatchingEngine unit tests.

#include "engine/matching_engine.hpp"

#include <gtest/gtest.h>
#include <span>

using namespace iex;

// Helpers — fixed-point prices: $1.00 = 10'000
static constexpr Price kP100 = 100'0000;  // $100.00
static constexpr Price kP101 = 101'0000;  // $101.00
static constexpr Price kP99  =  99'0000;  // $99.00

static constexpr SymbolId kSym = SymbolId{1};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineTest : public ::testing::Test {
protected:
    MatchingEngine engine{128};  // small pool for tests 

    void SetUp() override {
        engine.register_symbol(kSym);
    }

    std::expected<OrderId, EngineError> buy_limit(Price p, Quantity q) {
        return engine.submit(kSym, Side::kBuy, OrderType::kLimit, p, q);
    }
    std::expected<OrderId, EngineError> sell_limit(Price p, Quantity q) {
        return engine.submit(kSym, Side::kSell, OrderType::kLimit, p, q);
    }
    std::expected<OrderId, EngineError> buy_market(Quantity q) {
        return engine.submit(kSym, Side::kBuy, OrderType::kMarket, 0, q);
    }
    std::expected<OrderId, EngineError> sell_market(Quantity q) {
        return engine.submit(kSym, Side::kSell, OrderType::kMarket, 0, q);
    }
    std::expected<OrderId, EngineError> buy_ioc(Price p, Quantity q) {
        return engine.submit(kSym, Side::kBuy, OrderType::kIOC, p, q);
    }
};

// ---------------------------------------------------------------------------
// Input validation
// ---------------------------------------------------------------------------

TEST_F(EngineTest, ZeroQuantityReturnsError) {
    auto r = buy_limit(kP100, 0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kInvalidQuantity);
}

TEST_F(EngineTest, ZeroPriceLimitReturnsError) {
    auto r = engine.submit(kSym, Side::kBuy, OrderType::kLimit, 0, 100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kInvalidPrice);
}

TEST_F(EngineTest, NegativePriceLimitReturnsError) {
    auto r = engine.submit(kSym, Side::kBuy, OrderType::kLimit, -1, 100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kInvalidPrice);
}

TEST_F(EngineTest, UnknownSymbolReturnsError) {
    auto r = engine.submit(SymbolId{99}, Side::kBuy, OrderType::kLimit, kP100, 10);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kUnknownSymbol);
}

// ---------------------------------------------------------------------------
// Resting limit orders update BBO
// ---------------------------------------------------------------------------

TEST_F(EngineTest, RestingBidUpdatesBBO) {
    auto r = buy_limit(kP100, 50);
    ASSERT_TRUE(r.has_value());
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, kP100);
    EXPECT_EQ(bbo->bid_size,  Quantity{50});
    EXPECT_EQ(bbo->ask_price, Price{0});
}

TEST_F(EngineTest, RestingAskUpdatesBBO) {
    auto r = sell_limit(kP101, 30);
    ASSERT_TRUE(r.has_value());
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ask_price, kP101);
    EXPECT_EQ(bbo->ask_size,  Quantity{30});
    EXPECT_EQ(bbo->bid_price, Price{0});
}

// ---------------------------------------------------------------------------
// Two-sided limit match — exact fill
// ---------------------------------------------------------------------------

TEST_F(EngineTest, TwoSidedExactFill) {
    // Post a resting sell, then an aggressive buy at the same price.
    auto sell_id = sell_limit(kP100, 100);
    ASSERT_TRUE(sell_id.has_value());
    EXPECT_TRUE(engine.drain_trades().empty());  // no match yet

    auto buy_id = buy_limit(kP100, 100);
    ASSERT_TRUE(buy_id.has_value());

    std::span<const TradeEvent> trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price,    kP100);
    EXPECT_EQ(trades[0].quantity, Quantity{100});
    EXPECT_EQ(trades[0].passive_id,    *sell_id);
    EXPECT_EQ(trades[0].aggressive_id, *buy_id);

    // Book should be empty after full fill.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, Price{0});
    EXPECT_EQ(bbo->ask_price, Price{0});
}

// ---------------------------------------------------------------------------
// Partial fill — aggressor larger than resting
// ---------------------------------------------------------------------------

TEST_F(EngineTest, PartialFillAgressorLarger) {
    sell_limit(kP100, 40);                    // resting sell: 40 shares
    auto buy_id = buy_limit(kP100, 100);     // aggressive buy: 100 shares
    ASSERT_TRUE(buy_id.has_value());

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, Quantity{40});  // filled 40

    // 60 remaining buy should rest in the book.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, kP100);
    EXPECT_EQ(bbo->bid_size,  Quantity{60});
    EXPECT_EQ(bbo->ask_price, Price{0});  // sell fully consumed
}

// ---------------------------------------------------------------------------
// Partial fill — aggressor smaller than resting
// ---------------------------------------------------------------------------

TEST_F(EngineTest, PartialFillPassiveLarger) {
    sell_limit(kP100, 100);               // resting sell: 100 shares
    buy_limit(kP100, 40);                 // aggressive buy: 40 shares

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, Quantity{40});

    // 60 remaining sell should rest.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ask_price, kP100);
    EXPECT_EQ(bbo->ask_size,  Quantity{60});
    EXPECT_EQ(bbo->bid_price, Price{0});
}

// ---------------------------------------------------------------------------
// Multi-level sweep — buy sweeps through multiple ask levels
// ---------------------------------------------------------------------------

TEST_F(EngineTest, MultiLevelSweep) {
    sell_limit(kP100, 10);   // level 1
    sell_limit(kP101, 10);   // level 2
    // Buy aggressively at kP101 — should match both levels.
    buy_limit(kP101, 20);

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 2u);
    // Fills in price-time order: cheapest level first.
    EXPECT_EQ(trades[0].price,    kP100);
    EXPECT_EQ(trades[0].quantity, Quantity{10});
    EXPECT_EQ(trades[1].price,    kP101);
    EXPECT_EQ(trades[1].quantity, Quantity{10});

    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ask_price, Price{0});  // both levels consumed
}

// ---------------------------------------------------------------------------
// Price-priority: buy does not cross above its limit
// ---------------------------------------------------------------------------

TEST_F(EngineTest, LimitOrderDoesNotCrossAbovePrice) {
    sell_limit(kP101, 50);   // ask at 101
    buy_limit(kP100, 50);    // bid at 100 — does NOT cross 101
    EXPECT_TRUE(engine.drain_trades().empty());

    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, kP100);
    EXPECT_EQ(bbo->ask_price, kP101);
}

// ---------------------------------------------------------------------------
// Market order
// ---------------------------------------------------------------------------

TEST_F(EngineTest, MarketBuyFillsAtBestAsk) {
    sell_limit(kP100, 50);
    auto r = buy_market(30);
    ASSERT_TRUE(r.has_value());

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price,    kP100);
    EXPECT_EQ(trades[0].quantity, Quantity{30});
}

TEST_F(EngineTest, MarketBuyWithNoAsksProducesNoTrades) {
    // No resting sells — market buy has nothing to fill against.
    auto r = buy_market(50);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(engine.drain_trades().empty());
    // Unfilled market order is cancelled (not rested).
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, Price{0});
}

// ---------------------------------------------------------------------------
// IOC — remainder cancelled after partial fill
// ---------------------------------------------------------------------------

TEST_F(EngineTest, IOCDoesNotCrossAboveItsPrice) {
    // An IOC buy at kP100 must NOT fill against a kP101 ask.
    // This would have been wrong before the kMarket-only price bypass fix.
    sell_limit(kP101, 50);
    auto r = buy_ioc(kP100, 50);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(engine.drain_trades().empty());
    // IOC remainder (all 50) cancelled — book unchanged.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, Price{0});  // IOC did not rest
    EXPECT_EQ(bbo->ask_price, kP101);    // resting sell untouched
}

TEST_F(EngineTest, IOCPartialFillCancelsRemainder) {
    sell_limit(kP100, 30);                 // resting sell: 30
    auto r = buy_ioc(kP100, 80);          // IOC buy: 80, only 30 available
    ASSERT_TRUE(r.has_value());

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, Quantity{30});

    // Remainder (50) must not rest in the book.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, Price{0});
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST_F(EngineTest, CancelRemovedFromBook) {
    auto r = buy_limit(kP100, 50);
    ASSERT_TRUE(r.has_value());

    auto cancel_r = engine.cancel(*r);
    ASSERT_TRUE(cancel_r.has_value());

    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, Price{0});
}

TEST_F(EngineTest, CancelStaleIdReturnsError) {
    auto r = buy_limit(kP100, 50);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(engine.cancel(*r).has_value());  // cancel once

    // Second cancel with the same id should fail (id is stale after free).
    auto r2 = engine.cancel(*r);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), EngineError::kOrderNotFound);
}

// ---------------------------------------------------------------------------
// Pool exhaustion via engine
// ---------------------------------------------------------------------------

TEST_F(EngineTest, PoolExhaustionReturnsError) {
    // The fixture pool has 128 slots. Fill the book with resting bids.
    for (int i = 0; i < 128; ++i) {
        auto r = buy_limit(kP99, 1);
        ASSERT_TRUE(r.has_value()) << "alloc failed at i=" << i;
        // Don't cancel — hold the slot to drive exhaustion.
        (void)r;
    }
    // One more should exhaust the pool.
    auto r = buy_limit(kP99, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), EngineError::kPoolExhausted);
}

// ---------------------------------------------------------------------------
// FIFO within price level — first resting order fills first
// ---------------------------------------------------------------------------

TEST_F(EngineTest, FifoWithinPriceLevel) {
    // Two resting sells at the same price.
    auto s1 = sell_limit(kP100, 10);  // should fill first
    auto s2 = sell_limit(kP100, 20);  // should fill second
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());

    buy_limit(kP100, 15);  // aggressive buy: fills all of s1 (10) and 5 from s2

    auto trades = engine.drain_trades();
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].passive_id, *s1);
    EXPECT_EQ(trades[0].quantity,   Quantity{10});
    EXPECT_EQ(trades[1].passive_id, *s2);
    EXPECT_EQ(trades[1].quantity,   Quantity{5});

    // s2 has 15 remaining and should still rest.
    auto bbo = engine.get_bbo(kSym);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ask_size, Quantity{15});
}
