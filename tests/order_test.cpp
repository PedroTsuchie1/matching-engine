#include "matching_engine/order.hpp"

#include <gtest/gtest.h>

namespace matching_engine {
namespace {

TEST(OrderTest, CreatesLimitOrder) {
    const Order order = Order::limit(1, Side::Buy, 10'50, 100, 7);

    EXPECT_EQ(order.id(), 1);
    EXPECT_EQ(order.type(), OrderType::Limit);
    EXPECT_EQ(order.side(), Side::Buy);
    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'50);
    EXPECT_EQ(order.original_quantity(), 100);
    EXPECT_EQ(order.remaining_quantity(), 100);
    EXPECT_EQ(order.sequence(), 7);
    EXPECT_EQ(order.peg_reference(), PegReference::None);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, CreatesMarketOrderWithoutPrice) {
    const Order order = Order::market(2, Side::Sell, 75, 8);

    EXPECT_EQ(order.id(), 2);
    EXPECT_EQ(order.type(), OrderType::Market);
    EXPECT_EQ(order.side(), Side::Sell);
    EXPECT_FALSE(order.price().has_value());
    EXPECT_EQ(order.original_quantity(), 75);
    EXPECT_EQ(order.remaining_quantity(), 75);
    EXPECT_EQ(order.sequence(), 8);
    EXPECT_EQ(order.peg_reference(), PegReference::None);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, CreatesActivePeggedOrderWhenPriceIsAvailable) {
    const Order order = Order::pegged(
        3,
        Side::Buy,
        PegReference::Bid,
        50,
        9,
        10'25
    );

    EXPECT_EQ(order.id(), 3);
    EXPECT_EQ(order.type(), OrderType::Pegged);
    EXPECT_EQ(order.side(), Side::Buy);
    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'25);
    EXPECT_EQ(order.original_quantity(), 50);
    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.sequence(), 9);
    EXPECT_EQ(order.peg_reference(), PegReference::Bid);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, CreatesInactivePeggedOrderWithoutPrice) {
    const Order order = Order::pegged(
        4,
        Side::Sell,
        PegReference::Offer,
        25,
        10
    );

    EXPECT_EQ(order.id(), 4);
    EXPECT_EQ(order.type(), OrderType::Pegged);
    EXPECT_EQ(order.side(), Side::Sell);
    EXPECT_FALSE(order.price().has_value());
    EXPECT_EQ(order.original_quantity(), 25);
    EXPECT_EQ(order.remaining_quantity(), 25);
    EXPECT_EQ(order.sequence(), 10);
    EXPECT_EQ(order.peg_reference(), PegReference::Offer);
    EXPECT_EQ(order.status(), OrderStatus::Inactive);
}

}  // namespace
}  // namespace matching_engine
