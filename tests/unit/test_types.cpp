// Copyright (c) 2026 IEX Matching Engine Project
// Compile-time and runtime type sanity tests.
// Most guarantees are static_asserts in the headers; this file tests
// enum values and TradeEvent layout that benefit from an explicit test name.

#include "engine/types.hpp"
#include "engine/order_pool.hpp"  // Order struct

#include <gtest/gtest.h>
#include <type_traits>

using namespace iex;

// ---------------------------------------------------------------------------
// Sizes (runtime mirror of the static_asserts — named so failures are clear)
// ---------------------------------------------------------------------------

TEST(TypeSizes, ScalarAliases) {
    EXPECT_EQ(sizeof(Price),    8u);
    EXPECT_EQ(sizeof(Quantity), 4u);
    EXPECT_EQ(sizeof(OrderId),  8u);
    EXPECT_EQ(sizeof(SymbolId), 2u);
    EXPECT_EQ(sizeof(SeqNum),   8u);
}

TEST(TypeSizes, Enums) {
    EXPECT_EQ(sizeof(Side),          1u);
    EXPECT_EQ(sizeof(OrderType),     1u);
    EXPECT_EQ(sizeof(TradingStatus), 1u);
}

TEST(TypeSizes, BestBidOffer) {
    EXPECT_EQ(sizeof(BestBidOffer),  64u);
    EXPECT_EQ(alignof(BestBidOffer), 64u);
}

TEST(TypeSizes, TradeEvent) {
    EXPECT_EQ(sizeof(TradeEvent), 32u);
}

TEST(TypeSizes, Order) {
    EXPECT_EQ(sizeof(Order),  64u);
    EXPECT_EQ(alignof(Order), 64u);
}

// ---------------------------------------------------------------------------
// Enum values — ASCII literals used for log readability
// ---------------------------------------------------------------------------

TEST(EnumValues, Side) {
    EXPECT_EQ(static_cast<uint8_t>(Side::kBuy),  static_cast<uint8_t>('B'));
    EXPECT_EQ(static_cast<uint8_t>(Side::kSell), static_cast<uint8_t>('S'));
}

TEST(EnumValues, OrderType) {
    EXPECT_EQ(static_cast<uint8_t>(OrderType::kLimit),  static_cast<uint8_t>('L'));
    EXPECT_EQ(static_cast<uint8_t>(OrderType::kMarket), static_cast<uint8_t>('M'));
    EXPECT_EQ(static_cast<uint8_t>(OrderType::kIOC),    static_cast<uint8_t>('I'));
}

TEST(EnumValues, TradingStatus) {
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::kHalted),    static_cast<uint8_t>('H'));
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::kQuoteOnly), static_cast<uint8_t>('Q'));
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::kTrading),   static_cast<uint8_t>('T'));
}

// ---------------------------------------------------------------------------
// Trivially copyable — required for safe seqlock and ring-buffer memcpy
// ---------------------------------------------------------------------------

TEST(TriviallyCopyable, BestBidOffer) {
    EXPECT_TRUE(std::is_trivially_copyable_v<BestBidOffer>);
}

TEST(TriviallyCopyable, TradeEvent) {
    EXPECT_TRUE(std::is_trivially_copyable_v<TradeEvent>);
}

TEST(TriviallyCopyable, Order) {
    EXPECT_TRUE(std::is_trivially_copyable_v<Order>);
}

// ---------------------------------------------------------------------------
// BestBidOffer zero-init
// ---------------------------------------------------------------------------

TEST(BestBidOffer, ZeroInitialised) {
    BestBidOffer bbo{};
    EXPECT_EQ(bbo.bid_price, 0);
    EXPECT_EQ(bbo.bid_size,  0u);
    EXPECT_EQ(bbo.ask_price, 0);
    EXPECT_EQ(bbo.ask_size,  0u);
    EXPECT_EQ(bbo.symbol_id, 0u);
}
