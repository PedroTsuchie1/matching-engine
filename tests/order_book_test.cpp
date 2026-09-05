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

TEST(OrderBookTest, SupportsReadOnlyQueries) {
    OrderBook book;
    Order order = Order::limit(50, Side::Buy, 10'00, 10, 1);

    ASSERT_TRUE(book.add(order));

    const OrderBook& read_only_book = book;

    EXPECT_EQ(read_only_book.find(order.id()), &order);
    EXPECT_EQ(read_only_book.best(Side::Buy), &order);
    EXPECT_EQ(read_only_book.size(), 1);
    EXPECT_FALSE(read_only_book.empty());
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
    Order cancelled = Order::limit(2, Side::Buy, 10'00, 10, 2);

    ASSERT_TRUE(cancelled.cancel());

    EXPECT_FALSE(book.add(market));
    EXPECT_FALSE(book.add(cancelled));
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

TEST(OrderBookTest, FindsSellLimitReferenceWhileIgnoringPeggedOrders) {
    OrderBook book;
    Order peg = Order::pegged(
        3,
        Side::Sell,
        PegReference::Offer,
        10,
        3,
        9'50
    );
    Order limit = Order::limit(4, Side::Sell, 10'00, 10, 4);

    ASSERT_TRUE(book.add(peg));
    ASSERT_TRUE(book.add(limit));

    ASSERT_NE(book.best(Side::Sell), nullptr);
    EXPECT_EQ(book.best(Side::Sell)->id(), peg.id());

    const std::optional<Price> reference =
        book.best_limit_price(Side::Sell);

    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference.value(), 10'00);
}

TEST(OrderBookTest, CreatesEmptySnapshot) {
    const OrderBook book;

    const OrderBookSnapshot snapshot = book.snapshot();

    EXPECT_TRUE(snapshot.buys.empty());
    EXPECT_TRUE(snapshot.sells.empty());
}

TEST(OrderBookTest, SnapshotPreservesPriceTimePriority) {
    OrderBook book;

    Order lower_buy = Order::limit(
        1,
        Side::Buy,
        10'00,
        10,
        1
    );

    Order later_best_buy = Order::limit(
        2,
        Side::Buy,
        10'50,
        20,
        4
    );

    Order earlier_best_buy = Order::limit(
        3,
        Side::Buy,
        10'50,
        30,
        2
    );

    Order higher_sell = Order::limit(
        4,
        Side::Sell,
        11'00,
        40,
        3
    );

    Order lower_sell = Order::limit(
        5,
        Side::Sell,
        10'75,
        50,
        5
    );

    ASSERT_TRUE(book.add(lower_buy));
    ASSERT_TRUE(book.add(later_best_buy));
    ASSERT_TRUE(book.add(earlier_best_buy));
    ASSERT_TRUE(book.add(higher_sell));
    ASSERT_TRUE(book.add(lower_sell));

    const OrderBookSnapshot snapshot = book.snapshot();

    ASSERT_EQ(snapshot.buys.size(), 2);
    EXPECT_EQ(snapshot.buys[0].price, 10'50);
    EXPECT_EQ(snapshot.buys[0].total_quantity, 50);
    ASSERT_EQ(snapshot.buys[0].orders.size(), 2);
    EXPECT_EQ(snapshot.buys[0].orders[0].order_id, 3);
    EXPECT_EQ(snapshot.buys[0].orders[1].order_id, 2);

    EXPECT_EQ(snapshot.buys[1].price, 10'00);
    EXPECT_EQ(snapshot.buys[1].total_quantity, 10);
    ASSERT_EQ(snapshot.buys[1].orders.size(), 1);
    EXPECT_EQ(snapshot.buys[1].orders[0].order_id, 1);

    ASSERT_EQ(snapshot.sells.size(), 2);
    EXPECT_EQ(snapshot.sells[0].price, 10'75);
    EXPECT_EQ(snapshot.sells[0].total_quantity, 50);
    ASSERT_EQ(snapshot.sells[0].orders.size(), 1);
    EXPECT_EQ(snapshot.sells[0].orders[0].order_id, 5);

    EXPECT_EQ(snapshot.sells[1].price, 11'00);
    EXPECT_EQ(snapshot.sells[1].total_quantity, 40);
    ASSERT_EQ(snapshot.sells[1].orders.size(), 1);
    EXPECT_EQ(snapshot.sells[1].orders[0].order_id, 4);
}

TEST(OrderBookTest, SnapshotCopiesOrderDetails) {
    OrderBook book;

    Order limit = Order::limit(
        10,
        Side::Buy,
        10'00,
        25,
        1
    );

    Order peg = Order::pegged(
        11,
        Side::Buy,
        PegReference::Bid,
        15,
        2,
        10'00
    );

    ASSERT_TRUE(book.add(limit));
    ASSERT_TRUE(book.add(peg));
    ASSERT_TRUE(limit.apply_fill(5));

    const OrderBookSnapshot snapshot = book.snapshot();

    ASSERT_EQ(snapshot.buys.size(), 1);

    const BookLevelSnapshot& level = snapshot.buys[0];

    EXPECT_EQ(level.price, 10'00);
    EXPECT_EQ(level.total_quantity, 35);
    ASSERT_EQ(level.orders.size(), 2);

    EXPECT_EQ(level.orders[0].order_id, 10);
    EXPECT_EQ(level.orders[0].remaining_quantity, 20);
    EXPECT_EQ(level.orders[0].sequence, 1);
    EXPECT_FALSE(level.orders[0].peg_reference.has_value());

    EXPECT_EQ(level.orders[1].order_id, 11);
    EXPECT_EQ(level.orders[1].remaining_quantity, 15);
    EXPECT_EQ(level.orders[1].sequence, 2);
    ASSERT_TRUE(level.orders[1].peg_reference.has_value());
    EXPECT_EQ(
        level.orders[1].peg_reference.value(),
        PegReference::Bid
    );
}

}  // namespace
}  // namespace matching_engine
