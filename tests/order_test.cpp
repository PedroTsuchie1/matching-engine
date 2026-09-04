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

TEST(OrderTest, ActivatesInactivePeggedOrderAtReferencePrice) {
    Order order = Order::pegged(
        5,
        Side::Buy,
        PegReference::Bid,
        40,
        11
    );

    ASSERT_TRUE(order.apply_peg_reference(10'75));

    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'75);
    EXPECT_EQ(order.status(), OrderStatus::Active);
    EXPECT_EQ(order.id(), 5);
    EXPECT_EQ(order.remaining_quantity(), 40);
    EXPECT_EQ(order.sequence(), 11);
}

TEST(OrderTest, RepricesActivePeggedOrderWithoutChangingPriority) {
    Order order = Order::pegged(
        6,
        Side::Sell,
        PegReference::Offer,
        30,
        12,
        11'00
    );

    ASSERT_TRUE(order.apply_peg_reference(10'90));

    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'90);
    EXPECT_EQ(order.status(), OrderStatus::Active);
    EXPECT_EQ(order.remaining_quantity(), 30);
    EXPECT_EQ(order.sequence(), 12);
}

TEST(OrderTest, DeactivatesPeggedOrderWhenReferenceDisappears) {
    Order order = Order::pegged(
        7,
        Side::Buy,
        PegReference::Bid,
        20,
        13,
        10'50
    );

    ASSERT_TRUE(order.apply_peg_reference(std::nullopt));

    EXPECT_FALSE(order.price().has_value());
    EXPECT_EQ(order.status(), OrderStatus::Inactive);
    EXPECT_EQ(order.remaining_quantity(), 20);
    EXPECT_EQ(order.sequence(), 13);
}

TEST(OrderTest, RejectsPegReferenceApplicationForNonPeggedOrders) {
    Order limit = Order::limit(8, Side::Buy, 10'00, 10, 14);
    Order market = Order::market(9, Side::Sell, 10, 15);

    EXPECT_FALSE(limit.apply_peg_reference(10'25));
    EXPECT_FALSE(market.apply_peg_reference(10'25));

    ASSERT_TRUE(limit.price().has_value());
    EXPECT_EQ(limit.price().value(), 10'00);
    EXPECT_EQ(limit.status(), OrderStatus::Active);
    EXPECT_FALSE(market.price().has_value());
    EXPECT_EQ(market.status(), OrderStatus::Active);
}

TEST(OrderTest, CancelsActiveAndInactiveOrders) {
    Order limit = Order::limit(10, Side::Buy, 10'00, 10, 16);
    Order inactive_peg = Order::pegged(
        11,
        Side::Sell,
        PegReference::Offer,
        15,
        17
    );

    ASSERT_TRUE(limit.cancel());
    ASSERT_TRUE(inactive_peg.cancel());

    EXPECT_EQ(limit.status(), OrderStatus::Cancelled);
    EXPECT_EQ(limit.remaining_quantity(), 0);
    EXPECT_EQ(inactive_peg.status(), OrderStatus::Cancelled);
    EXPECT_EQ(inactive_peg.remaining_quantity(), 0);
}

TEST(OrderTest, RejectsTransitionsAfterCancellation) {
    Order order = Order::pegged(
        12,
        Side::Buy,
        PegReference::Bid,
        10,
        18,
        10'00
    );

    ASSERT_TRUE(order.cancel());

    EXPECT_FALSE(order.cancel());
    EXPECT_FALSE(order.apply_peg_reference(10'50));
    EXPECT_EQ(order.status(), OrderStatus::Cancelled);
    EXPECT_EQ(order.remaining_quantity(), 0);
    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'00);
}

}  // namespace
}  // namespace matching_engine
