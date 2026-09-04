#include "matching_engine/matching_engine.hpp"

#include <gtest/gtest.h>

namespace matching_engine {
namespace {

TEST(MatchingEngineTest, StartsEmpty) {
    const MatchingEngine engine;

    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_EQ(engine.find_order(1), nullptr);

    const OrderBook& book = engine.order_book();

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.size(), 0);
    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Sell), nullptr);
}

TEST(MatchingEngineTest, RejectsNegativeQuantity) {
    MatchingEngine engine;

    const SubmissionResult result = engine.submit_limit(
        Side::Buy,
        10'00,
        -10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, AcceptsLimitOrderAndRestsItInEmptyBook) {
    MatchingEngine engine;

    const SubmissionResult result = engine.submit_limit(
        Side::Buy,
        10'50,
        100
    );

    const SubmissionReport* report =
        std::get_if<SubmissionReport>(&result);

    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->order_id, 1);
    EXPECT_TRUE(report->trades.empty());
    EXPECT_TRUE(report->cancelled_order_ids.empty());

    EXPECT_EQ(engine.order_count(), 1);

    const Order* order = engine.find_order(report->order_id);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->type(), OrderType::Limit);
    EXPECT_EQ(order->side(), Side::Buy);
    EXPECT_EQ(order->price(), 10'50);
    EXPECT_EQ(order->original_quantity(), 100);
    EXPECT_EQ(order->remaining_quantity(), 100);
    EXPECT_EQ(order->status(), OrderStatus::Active);

    const OrderBook& book = engine.order_book();

    EXPECT_EQ(book.size(), 1);
    EXPECT_EQ(book.best(Side::Buy), order);
    EXPECT_EQ(book.best(Side::Sell), nullptr);
}

TEST(MatchingEngineTest, RejectsInvalidInputsWithoutConsumingOrderId) {
    MatchingEngine engine;

    const SubmissionResult zero_price = engine.submit_limit(
        Side::Buy,
        0,
        10
    );
    const SubmissionResult negative_price = engine.submit_limit(
        Side::Buy,
        -10'00,
        10
    );
    const SubmissionResult zero_quantity = engine.submit_limit(
        Side::Buy,
        10'00,
        0
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_price));
    ASSERT_TRUE(std::holds_alternative<EngineError>(negative_price));
    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_quantity));
    EXPECT_EQ(
        std::get<EngineError>(zero_price),
        EngineError::InvalidPrice
    );
    EXPECT_EQ(
        std::get<EngineError>(negative_price),
        EngineError::InvalidPrice
    );
    EXPECT_EQ(
        std::get<EngineError>(zero_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());

    const SubmissionResult valid_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionReport* valid_report =
        std::get_if<SubmissionReport>(&valid_result);

    ASSERT_NE(valid_report, nullptr);
    EXPECT_EQ(valid_report->order_id, 1);
}

TEST(MatchingEngineTest, RestsOppositeOrdersWhenPricesDoNotCross) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        11'00,
        40
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'50,
        25
    );

    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);
    EXPECT_TRUE(sell_report->trades.empty());
    EXPECT_TRUE(buy_report->trades.empty());
    EXPECT_EQ(engine.order_count(), 2);
    EXPECT_EQ(engine.order_book().size(), 2);

    const Order* best_buy = engine.order_book().best(Side::Buy);
    const Order* best_sell = engine.order_book().best(Side::Sell);

    ASSERT_NE(best_buy, nullptr);
    ASSERT_NE(best_sell, nullptr);
    EXPECT_EQ(best_buy->id(), buy_report->order_id);
    EXPECT_EQ(best_sell->id(), sell_report->order_id);
}

TEST(MatchingEngineTest, FullyMatchesOrdersWithEqualQuantities) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        40
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'50,
        40
    );

    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);
    ASSERT_EQ(buy_report->trades.size(), 1);

    const Trade& trade = buy_report->trades.front();

    EXPECT_EQ(trade.resting_order_id, sell_report->order_id);
    EXPECT_EQ(trade.aggressive_order_id, buy_report->order_id);
    EXPECT_EQ(trade.price, 10'00);
    EXPECT_EQ(trade.quantity, 40);

    const Order* sell_order = engine.find_order(sell_report->order_id);
    const Order* buy_order = engine.find_order(buy_report->order_id);

    ASSERT_NE(sell_order, nullptr);
    ASSERT_NE(buy_order, nullptr);
    EXPECT_EQ(sell_order->status(), OrderStatus::Filled);
    EXPECT_EQ(buy_order->status(), OrderStatus::Filled);
    EXPECT_EQ(sell_order->remaining_quantity(), 0);
    EXPECT_EQ(buy_order->remaining_quantity(), 0);
    EXPECT_EQ(engine.order_count(), 2);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RestsAggressiveRemainderAfterPartialMatch) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        40
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        100
    );
    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);
    ASSERT_EQ(buy_report->trades.size(), 1);
    EXPECT_EQ(buy_report->trades.front().quantity, 40);

    const Order* sell_order = engine.find_order(sell_report->order_id);
    const Order* buy_order = engine.find_order(buy_report->order_id);

    ASSERT_NE(sell_order, nullptr);
    ASSERT_NE(buy_order, nullptr);
    EXPECT_EQ(sell_order->status(), OrderStatus::Filled);
    EXPECT_EQ(buy_order->status(), OrderStatus::Active);
    EXPECT_EQ(buy_order->remaining_quantity(), 60);
    EXPECT_EQ(engine.order_book().size(), 1);
    EXPECT_EQ(engine.order_book().best(Side::Buy), buy_order);
    EXPECT_EQ(engine.order_book().best(Side::Sell), nullptr);
}

TEST(MatchingEngineTest, KeepsRestingRemainderAfterPartialMatch) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        100
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'50,
        40
    );
    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);
    ASSERT_EQ(buy_report->trades.size(), 1);
    EXPECT_EQ(buy_report->trades.front().quantity, 40);

    const Order* sell_order = engine.find_order(sell_report->order_id);
    const Order* buy_order = engine.find_order(buy_report->order_id);

    ASSERT_NE(sell_order, nullptr);
    ASSERT_NE(buy_order, nullptr);
    EXPECT_EQ(sell_order->status(), OrderStatus::Active);
    EXPECT_EQ(sell_order->remaining_quantity(), 60);
    EXPECT_EQ(buy_order->status(), OrderStatus::Filled);
    EXPECT_EQ(engine.order_book().size(), 1);
    EXPECT_EQ(engine.order_book().best(Side::Sell), sell_order);
    EXPECT_EQ(engine.order_book().best(Side::Buy), nullptr);
}

TEST(MatchingEngineTest, MatchesBestPricesBeforeWorsePrices) {
    MatchingEngine engine;

    const SubmissionResult worse_sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionResult better_sell_result = engine.submit_limit(
        Side::Sell,
        9'50,
        30
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        45
    );

    const SubmissionReport* worse_sell =
        std::get_if<SubmissionReport>(&worse_sell_result);
    const SubmissionReport* better_sell =
        std::get_if<SubmissionReport>(&better_sell_result);
    const SubmissionReport* buy =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(worse_sell, nullptr);
    ASSERT_NE(better_sell, nullptr);
    ASSERT_NE(buy, nullptr);
    ASSERT_EQ(buy->trades.size(), 2);

    EXPECT_EQ(
        buy->trades[0].resting_order_id,
        better_sell->order_id
    );
    EXPECT_EQ(buy->trades[0].price, 9'50);
    EXPECT_EQ(buy->trades[0].quantity, 30);
    EXPECT_EQ(
        buy->trades[1].resting_order_id,
        worse_sell->order_id
    );
    EXPECT_EQ(buy->trades[1].price, 10'00);
    EXPECT_EQ(buy->trades[1].quantity, 15);

    const Order* remaining_sell =
        engine.find_order(worse_sell->order_id);

    ASSERT_NE(remaining_sell, nullptr);
    EXPECT_EQ(remaining_sell->remaining_quantity(), 5);
    EXPECT_EQ(remaining_sell->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Sell), remaining_sell);
}

TEST(MatchingEngineTest, PreservesFifoWithinTheSamePrice) {
    MatchingEngine engine;

    const SubmissionResult first_sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionResult second_sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        25
    );

    const SubmissionReport* first_sell =
        std::get_if<SubmissionReport>(&first_sell_result);
    const SubmissionReport* second_sell =
        std::get_if<SubmissionReport>(&second_sell_result);
    const SubmissionReport* buy =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(first_sell, nullptr);
    ASSERT_NE(second_sell, nullptr);
    ASSERT_NE(buy, nullptr);
    ASSERT_EQ(buy->trades.size(), 2);
    EXPECT_EQ(
        buy->trades[0].resting_order_id,
        first_sell->order_id
    );
    EXPECT_EQ(buy->trades[0].quantity, 20);
    EXPECT_EQ(
        buy->trades[1].resting_order_id,
        second_sell->order_id
    );
    EXPECT_EQ(buy->trades[1].quantity, 5);

    const Order* second_sell_order =
        engine.find_order(second_sell->order_id);

    ASSERT_NE(second_sell_order, nullptr);
    EXPECT_EQ(second_sell_order->remaining_quantity(), 15);
    EXPECT_EQ(engine.order_book().best(Side::Sell), second_sell_order);
}

TEST(MatchingEngineTest, StopsMatchingWhenNextPriceDoesNotCrossLimit) {
    MatchingEngine engine;

    const SubmissionResult crossing_sell_result = engine.submit_limit(
        Side::Sell,
        9'50,
        10
    );
    const SubmissionResult expensive_sell_result = engine.submit_limit(
        Side::Sell,
        10'50,
        10
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        30
    );

    const SubmissionReport* crossing_sell =
        std::get_if<SubmissionReport>(&crossing_sell_result);
    const SubmissionReport* expensive_sell =
        std::get_if<SubmissionReport>(&expensive_sell_result);
    const SubmissionReport* buy =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(crossing_sell, nullptr);
    ASSERT_NE(expensive_sell, nullptr);
    ASSERT_NE(buy, nullptr);
    ASSERT_EQ(buy->trades.size(), 1);
    EXPECT_EQ(
        buy->trades.front().resting_order_id,
        crossing_sell->order_id
    );
    EXPECT_EQ(buy->trades.front().quantity, 10);

    const Order* buy_order = engine.find_order(buy->order_id);
    const Order* expensive_sell_order =
        engine.find_order(expensive_sell->order_id);

    ASSERT_NE(buy_order, nullptr);
    ASSERT_NE(expensive_sell_order, nullptr);
    EXPECT_EQ(buy_order->remaining_quantity(), 20);
    EXPECT_EQ(expensive_sell_order->remaining_quantity(), 10);
    EXPECT_EQ(engine.order_book().size(), 2);
    EXPECT_EQ(engine.order_book().best(Side::Buy), buy_order);
    EXPECT_EQ(engine.order_book().best(Side::Sell), expensive_sell_order);
}

TEST(MatchingEngineTest, SellAggressorTradesAtRestingBuyPrice) {
    MatchingEngine engine;

    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'50,
        30
    );
    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        10
    );

    const SubmissionReport* buy =
        std::get_if<SubmissionReport>(&buy_result);
    const SubmissionReport* sell =
        std::get_if<SubmissionReport>(&sell_result);

    ASSERT_NE(buy, nullptr);
    ASSERT_NE(sell, nullptr);
    ASSERT_EQ(sell->trades.size(), 1);
    EXPECT_EQ(sell->trades.front().resting_order_id, buy->order_id);
    EXPECT_EQ(sell->trades.front().aggressive_order_id, sell->order_id);
    EXPECT_EQ(sell->trades.front().price, 10'50);
    EXPECT_EQ(sell->trades.front().quantity, 10);

    const Order* resting_buy = engine.find_order(buy->order_id);
    const Order* aggressive_sell = engine.find_order(sell->order_id);

    ASSERT_NE(resting_buy, nullptr);
    ASSERT_NE(aggressive_sell, nullptr);
    EXPECT_EQ(resting_buy->remaining_quantity(), 20);
    EXPECT_EQ(resting_buy->status(), OrderStatus::Active);
    EXPECT_EQ(aggressive_sell->status(), OrderStatus::Filled);
    EXPECT_EQ(engine.order_book().best(Side::Buy), resting_buy);
    EXPECT_EQ(engine.order_book().best(Side::Sell), nullptr);
}

}  // namespace
}  // namespace matching_engine
