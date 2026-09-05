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
    EXPECT_FALSE(order.peg_reference().has_value());
    EXPECT_FALSE(order.is_pegged());
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
    EXPECT_FALSE(order.peg_reference().has_value());
    EXPECT_FALSE(order.is_pegged());
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, CreatesActivePeggedOrderAtReferencePrice) {
    const Order order = Order::pegged(
        3,
        Side::Buy,
        PegReference::Bid,
        50,
        9,
        10'25
    );

    EXPECT_EQ(order.id(), 3);
    EXPECT_EQ(order.type(), OrderType::Limit);
    EXPECT_EQ(order.side(), Side::Buy);
    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'25);
    EXPECT_EQ(order.original_quantity(), 50);
    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.sequence(), 9);
    ASSERT_TRUE(order.peg_reference().has_value());
    EXPECT_EQ(order.peg_reference().value(), PegReference::Bid);
    EXPECT_TRUE(order.is_pegged());
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RepricesActivePeggedOrderAndKeepsPriority) {
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
    ASSERT_TRUE(order.peg_reference().has_value());
    EXPECT_EQ(order.peg_reference().value(), PegReference::Offer);
    EXPECT_TRUE(order.is_pegged());
}

TEST(OrderTest, RejectsPegReferenceApplicationForNonPeggedOrders) {
    Order limit = Order::limit(8, Side::Buy, 10'00, 10, 14);
    Order market = Order::market(9, Side::Sell, 10, 15);

    EXPECT_FALSE(limit.apply_peg_reference(10'25));
    EXPECT_FALSE(market.apply_peg_reference(10'25));

    ASSERT_TRUE(limit.price().has_value());
    EXPECT_EQ(limit.price().value(), 10'00);
    EXPECT_EQ(limit.status(), OrderStatus::Active);
    EXPECT_EQ(limit.sequence(), 14);
    EXPECT_FALSE(limit.is_pegged());
    EXPECT_FALSE(market.price().has_value());
    EXPECT_EQ(market.status(), OrderStatus::Active);
    EXPECT_EQ(market.sequence(), 15);
    EXPECT_FALSE(market.is_pegged());
}

TEST(OrderTest, AppliesQuantityAmendmentWithProvidedPriority) {
    Order order = Order::limit(20, Side::Buy, 10'00, 50, 30);

    ASSERT_TRUE(order.apply_quantity_amendment(75, 40));

    EXPECT_EQ(order.original_quantity(), 50);
    EXPECT_EQ(order.remaining_quantity(), 75);
    EXPECT_EQ(order.sequence(), 40);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RejectsNonPositiveQuantityAmendment) {
    Order order = Order::limit(21, Side::Buy, 10'00, 50, 31);

    EXPECT_FALSE(order.apply_quantity_amendment(0, 41));
    EXPECT_FALSE(order.apply_quantity_amendment(-10, 42));

    EXPECT_EQ(order.original_quantity(), 50);
    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.sequence(), 31);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RejectsQuantityAmendmentForClosedOrder) {
    Order order = Order::limit(22, Side::Sell, 10'00, 25, 32);

    ASSERT_TRUE(order.cancel());
    EXPECT_FALSE(order.apply_quantity_amendment(20, 43));

    EXPECT_EQ(order.original_quantity(), 25);
    EXPECT_EQ(order.remaining_quantity(), 0);
    EXPECT_EQ(order.sequence(), 32);
    EXPECT_EQ(order.status(), OrderStatus::Cancelled);
}

TEST(OrderTest, AppliesPriceAmendmentWithProvidedPriority) {
    Order order = Order::limit(23, Side::Buy, 10'00, 50, 33);

    ASSERT_TRUE(order.apply_price_amendment(10'25, 44));

    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'25);
    EXPECT_EQ(order.original_quantity(), 50);
    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.sequence(), 44);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RejectsInvalidOrUnsupportedPriceAmendment) {
    Order limit = Order::limit(24, Side::Buy, 10'00, 50, 34);
    Order market = Order::market(25, Side::Sell, 50, 35);
    Order peg = Order::pegged(
        26,
        Side::Buy,
        PegReference::Bid,
        50,
        36,
        10'00
    );

    EXPECT_FALSE(limit.apply_price_amendment(0, 45));
    EXPECT_FALSE(limit.apply_price_amendment(-10, 46));
    EXPECT_FALSE(market.apply_price_amendment(10'25, 47));
    EXPECT_FALSE(peg.apply_price_amendment(10'25, 48));

    ASSERT_TRUE(limit.price().has_value());
    EXPECT_EQ(limit.price().value(), 10'00);
    EXPECT_EQ(limit.sequence(), 34);
    EXPECT_FALSE(market.price().has_value());
    EXPECT_EQ(market.sequence(), 35);
    ASSERT_TRUE(peg.price().has_value());
    EXPECT_EQ(peg.price().value(), 10'00);
    EXPECT_EQ(peg.sequence(), 36);
}

TEST(OrderTest, RejectsPriceAmendmentForClosedOrder) {
    Order order = Order::limit(27, Side::Sell, 10'00, 25, 37);

    ASSERT_TRUE(order.cancel());
    EXPECT_FALSE(order.apply_price_amendment(10'25, 49));

    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'00);
    EXPECT_EQ(order.sequence(), 37);
    EXPECT_EQ(order.status(), OrderStatus::Cancelled);
}

TEST(OrderTest, CancelsActiveOrders) {
    Order limit = Order::limit(10, Side::Buy, 10'00, 10, 16);
    Order peg = Order::pegged(
        11,
        Side::Sell,
        PegReference::Offer,
        15,
        17,
        10'25
    );

    ASSERT_TRUE(limit.cancel());
    ASSERT_TRUE(peg.cancel());

    EXPECT_EQ(limit.status(), OrderStatus::Cancelled);
    EXPECT_EQ(limit.remaining_quantity(), 0);
    EXPECT_EQ(peg.status(), OrderStatus::Cancelled);
    EXPECT_EQ(peg.remaining_quantity(), 0);
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
    EXPECT_EQ(order.sequence(), 18);
    ASSERT_TRUE(order.price().has_value());
    EXPECT_EQ(order.price().value(), 10'00);
}

TEST(OrderTest, AppliesPartialFillAndKeepsOrderActive) {
    Order order = Order::limit(13, Side::Buy, 10'00, 100, 19);

    ASSERT_TRUE(order.apply_fill(40));

    EXPECT_EQ(order.remaining_quantity(), 60);
    EXPECT_EQ(order.status(), OrderStatus::Active);
    EXPECT_EQ(order.original_quantity(), 100);
}

TEST(OrderTest, AppliesCompleteFillAndMarksOrderFilled) {
    Order order = Order::limit(14, Side::Sell, 10'00, 75, 20);

    ASSERT_TRUE(order.apply_fill(75));

    EXPECT_EQ(order.remaining_quantity(), 0);
    EXPECT_EQ(order.status(), OrderStatus::Filled);
    EXPECT_EQ(order.original_quantity(), 75);
}

TEST(OrderTest, RejectsZeroAndExcessiveFillQuantities) {
    Order order = Order::limit(15, Side::Buy, 10'00, 50, 21);

    EXPECT_FALSE(order.apply_fill(0));
    EXPECT_FALSE(order.apply_fill(51));

    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RejectsNegativeFillQuantity) {
    Order order = Order::limit(19, Side::Buy, 10'00, 50, 25);

    EXPECT_FALSE(order.apply_fill(-10));

    EXPECT_EQ(order.remaining_quantity(), 50);
    EXPECT_EQ(order.status(), OrderStatus::Active);
}

TEST(OrderTest, RejectsFillForCancelledOrder) {
    Order order = Order::limit(17, Side::Sell, 10'00, 25, 23);
    ASSERT_TRUE(order.cancel());

    EXPECT_FALSE(order.apply_fill(10));

    EXPECT_EQ(order.remaining_quantity(), 0);
    EXPECT_EQ(order.status(), OrderStatus::Cancelled);
}

TEST(OrderTest, RejectsAdditionalFillAfterOrderIsFilled) {
    Order order = Order::market(18, Side::Buy, 20, 24);
    ASSERT_TRUE(order.apply_fill(20));

    EXPECT_FALSE(order.apply_fill(1));

    EXPECT_EQ(order.remaining_quantity(), 0);
    EXPECT_EQ(order.status(), OrderStatus::Filled);
}

}  // namespace
}  // namespace matching_engine
