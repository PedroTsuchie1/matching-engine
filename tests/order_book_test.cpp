#include "matching_engine/order_book.hpp"

#include <gtest/gtest.h>

namespace matching_engine {
namespace {

TEST(OrderBookTest, StartsEmpty) {
    OrderBook book;

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.size(), 0);
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Sell), nullptr);
    EXPECT_FALSE(book.best_limit_price(Side::Buy).has_value());
    EXPECT_FALSE(book.best_limit_price(Side::Sell).has_value());
}

TEST(OrderBookTest, PrioritizesBuyOrdersByPriceThenSequence) {
    OrderBook book;
    Order lower_price = Order::limit(1, Side::Buy, 10'00, 10, 1);
    Order later_at_best_price = Order::limit(2, Side::Buy, 10'50, 10, 3);
    Order earlier_at_best_price = Order::limit(3, Side::Buy, 10'50, 10, 2);

    ASSERT_TRUE(book.add(lower_price));
    ASSERT_TRUE(book.add(later_at_best_price));
    ASSERT_TRUE(book.add(earlier_at_best_price));

    ASSERT_NE(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Buy)->id(), earlier_at_best_price.id());

    ASSERT_TRUE(book.remove(earlier_at_best_price.id()));
    ASSERT_NE(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Buy)->id(), later_at_best_price.id());
}

TEST(OrderBookTest, PrioritizesSellOrdersByLowestPrice) {
    OrderBook book;
    Order higher_price = Order::limit(1, Side::Sell, 10'50, 10, 1);
    Order lower_price = Order::limit(2, Side::Sell, 10'00, 10, 2);

    ASSERT_TRUE(book.add(higher_price));
    ASSERT_TRUE(book.add(lower_price));

    ASSERT_NE(book.best(Side::Sell), nullptr);
    EXPECT_EQ(book.best(Side::Sell)->id(), lower_price.id());
}

TEST(OrderBookTest, FindsAndRemovesActiveOrders) {
    OrderBook book;
    Order order = Order::limit(42, Side::Buy, 10'00, 25, 1);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.size(), 1);
    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_FALSE(book.add(order));

    EXPECT_TRUE(book.remove(order.id()));
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_FALSE(book.remove(order.id()));
}

TEST(OrderBookTest, RejectsOrdersThatCannotRest) {
    OrderBook book;
    Order market = Order::market(1, Side::Buy, 10, 1);
    Order inactive_peg = Order::pegged(
        2,
        Side::Buy,
        PegReference::Bid,
        10,
        2
    );

    EXPECT_FALSE(book.add(market));
    EXPECT_FALSE(book.add(inactive_peg));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, FindsLimitReferenceWhileIgnoringPeggedOrders) {
    OrderBook book;
    Order stale_peg = Order::pegged(
        1,
        Side::Buy,
        PegReference::Bid,
        10,
        1,
        10'50
    );
    Order limit = Order::limit(2, Side::Buy, 10'00, 10, 2);

    ASSERT_TRUE(book.add(stale_peg));
    ASSERT_TRUE(book.add(limit));

    ASSERT_NE(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Buy)->id(), stale_peg.id());

    const std::optional<Price> reference =
        book.best_limit_price(Side::Buy);

    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference.value(), 10'00);
}

}  // namespace
}  // namespace matching_engine
