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
    EXPECT_TRUE(report->cancellations.empty());

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

TEST(MatchingEngineTest, RejectsInvalidMarketQuantityWithoutConsumingOrderId) {
    MatchingEngine engine;

    const SubmissionResult zero_quantity = engine.submit_market(
        Side::Buy,
        0
    );
    const SubmissionResult negative_quantity = engine.submit_market(
        Side::Sell,
        -10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_quantity));
    ASSERT_TRUE(std::holds_alternative<EngineError>(negative_quantity));
    EXPECT_EQ(
        std::get<EngineError>(zero_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(
        std::get<EngineError>(negative_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());

    const SubmissionResult valid_limit = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionReport* report =
        std::get_if<SubmissionReport>(&valid_limit);

    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->order_id, 1);
}

TEST(MatchingEngineTest, CancelsMarketOrderWhenBookIsEmpty) {
    MatchingEngine engine;

    const SubmissionResult result = engine.submit_market(
        Side::Buy,
        50
    );
    const SubmissionReport* report =
        std::get_if<SubmissionReport>(&result);

    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->order_id, 1);
    EXPECT_TRUE(report->trades.empty());
    ASSERT_EQ(report->cancellations.size(), 1);
    EXPECT_EQ(report->cancellations.front().order_id, report->order_id);
    EXPECT_EQ(report->cancellations.front().quantity, 50);
    EXPECT_EQ(
        report->cancellations.front().reason,
        CancellationReason::MarketRemainder
    );

    const Order* market_order = engine.find_order(report->order_id);

    ASSERT_NE(market_order, nullptr);
    EXPECT_EQ(market_order->type(), OrderType::Market);
    EXPECT_FALSE(market_order->price().has_value());
    EXPECT_EQ(market_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(market_order->remaining_quantity(), 0);
    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, FullyFillsMarketOrderAgainstRestingLiquidity) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'25,
        30
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        30
    );

    const SubmissionReport* sell =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* market =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(sell, nullptr);
    ASSERT_NE(market, nullptr);
    ASSERT_EQ(market->trades.size(), 1);
    EXPECT_EQ(market->trades.front().resting_order_id, sell->order_id);
    EXPECT_EQ(market->trades.front().aggressive_order_id, market->order_id);
    EXPECT_EQ(market->trades.front().price, 10'25);
    EXPECT_EQ(market->trades.front().quantity, 30);
    EXPECT_TRUE(market->cancellations.empty());

    const Order* sell_order = engine.find_order(sell->order_id);
    const Order* market_order = engine.find_order(market->order_id);

    ASSERT_NE(sell_order, nullptr);
    ASSERT_NE(market_order, nullptr);
    EXPECT_EQ(sell_order->status(), OrderStatus::Filled);
    EXPECT_EQ(market_order->status(), OrderStatus::Filled);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, CancelsUnfilledMarketRemainderAfterPartialFill) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        15
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        40
    );

    const SubmissionReport* sell =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* market =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(sell, nullptr);
    ASSERT_NE(market, nullptr);
    ASSERT_EQ(market->trades.size(), 1);
    EXPECT_EQ(market->trades.front().quantity, 15);
    ASSERT_EQ(market->cancellations.size(), 1);
    EXPECT_EQ(market->cancellations.front().order_id, market->order_id);
    EXPECT_EQ(market->cancellations.front().quantity, 25);
    EXPECT_EQ(
        market->cancellations.front().reason,
        CancellationReason::MarketRemainder
    );

    const Order* sell_order = engine.find_order(sell->order_id);
    const Order* market_order = engine.find_order(market->order_id);

    ASSERT_NE(sell_order, nullptr);
    ASSERT_NE(market_order, nullptr);
    EXPECT_EQ(sell_order->status(), OrderStatus::Filled);
    EXPECT_EQ(market_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(market_order->remaining_quantity(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, MarketBuyConsumesMultipleSellPricesInPriorityOrder) {
    MatchingEngine engine;

    const SubmissionResult worse_sell_result = engine.submit_limit(
        Side::Sell,
        15'00,
        10
    );
    const SubmissionResult better_sell_result = engine.submit_limit(
        Side::Sell,
        9'50,
        20
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        25
    );

    const SubmissionReport* worse_sell =
        std::get_if<SubmissionReport>(&worse_sell_result);
    const SubmissionReport* better_sell =
        std::get_if<SubmissionReport>(&better_sell_result);
    const SubmissionReport* market =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(worse_sell, nullptr);
    ASSERT_NE(better_sell, nullptr);
    ASSERT_NE(market, nullptr);
    ASSERT_EQ(market->trades.size(), 2);
    EXPECT_EQ(
        market->trades[0].resting_order_id,
        better_sell->order_id
    );
    EXPECT_EQ(market->trades[0].price, 9'50);
    EXPECT_EQ(market->trades[0].quantity, 20);
    EXPECT_EQ(
        market->trades[1].resting_order_id,
        worse_sell->order_id
    );
    EXPECT_EQ(market->trades[1].price, 15'00);
    EXPECT_EQ(market->trades[1].quantity, 5);
    EXPECT_TRUE(market->cancellations.empty());

    const Order* remaining_sell =
        engine.find_order(worse_sell->order_id);
    const Order* market_order =
        engine.find_order(market->order_id);

    ASSERT_NE(remaining_sell, nullptr);
    ASSERT_NE(market_order, nullptr);
    EXPECT_EQ(remaining_sell->remaining_quantity(), 5);
    EXPECT_EQ(remaining_sell->status(), OrderStatus::Active);
    EXPECT_EQ(market_order->status(), OrderStatus::Filled);
    EXPECT_EQ(engine.order_book().size(), 1);
    EXPECT_EQ(engine.order_book().best(Side::Sell), remaining_sell);
    EXPECT_EQ(engine.order_book().best(Side::Buy), nullptr);
}

TEST(MatchingEngineTest, MarketSellConsumesBestBuyFirst) {
    MatchingEngine engine;

    const SubmissionResult worse_buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionResult better_buy_result = engine.submit_limit(
        Side::Buy,
        10'50,
        10
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Sell,
        15
    );

    const SubmissionReport* worse_buy =
        std::get_if<SubmissionReport>(&worse_buy_result);
    const SubmissionReport* better_buy =
        std::get_if<SubmissionReport>(&better_buy_result);
    const SubmissionReport* market =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(worse_buy, nullptr);
    ASSERT_NE(better_buy, nullptr);
    ASSERT_NE(market, nullptr);
    ASSERT_EQ(market->trades.size(), 2);
    EXPECT_EQ(
        market->trades[0].resting_order_id,
        better_buy->order_id
    );
    EXPECT_EQ(market->trades[0].price, 10'50);
    EXPECT_EQ(market->trades[0].quantity, 10);
    EXPECT_EQ(
        market->trades[1].resting_order_id,
        worse_buy->order_id
    );
    EXPECT_EQ(market->trades[1].price, 10'00);
    EXPECT_EQ(market->trades[1].quantity, 5);
    EXPECT_TRUE(market->cancellations.empty());

    const Order* remaining_buy =
        engine.find_order(worse_buy->order_id);

    ASSERT_NE(remaining_buy, nullptr);
    EXPECT_EQ(remaining_buy->remaining_quantity(), 5);
    EXPECT_EQ(remaining_buy->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Buy), remaining_buy);
    EXPECT_EQ(engine.order_book().best(Side::Sell), nullptr);
}

TEST(MatchingEngineTest, MarketOrderDoesNotMatchSameSideLiquidity) {
    MatchingEngine engine;

    const SubmissionResult limit_result = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        10
    );

    const SubmissionReport* limit =
        std::get_if<SubmissionReport>(&limit_result);
    const SubmissionReport* market =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(limit, nullptr);
    ASSERT_NE(market, nullptr);
    EXPECT_TRUE(market->trades.empty());
    ASSERT_EQ(market->cancellations.size(), 1);
    EXPECT_EQ(market->cancellations.front().order_id, market->order_id);

    const Order* limit_order = engine.find_order(limit->order_id);
    const Order* market_order = engine.find_order(market->order_id);

    ASSERT_NE(limit_order, nullptr);
    ASSERT_NE(market_order, nullptr);
    EXPECT_EQ(limit_order->status(), OrderStatus::Active);
    EXPECT_EQ(limit_order->remaining_quantity(), 20);
    EXPECT_EQ(market_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(engine.order_book().size(), 1);
    EXPECT_EQ(engine.order_book().best(Side::Buy), limit_order);
    EXPECT_EQ(engine.order_book().best(Side::Sell), nullptr);
}

TEST(MatchingEngineTest, RejectsInvalidPegInputsWithoutConsumingOrderId) {
    MatchingEngine engine;

    const SubmissionResult zero_quantity = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        0
    );
    const SubmissionResult negative_quantity = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        -10
    );
    const SubmissionResult bid_sell = engine.submit_peg(
        Side::Sell,
        PegReference::Bid,
        10
    );
    const SubmissionResult offer_buy = engine.submit_peg(
        Side::Buy,
        PegReference::Offer,
        10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_quantity));
    ASSERT_TRUE(std::holds_alternative<EngineError>(negative_quantity));
    ASSERT_TRUE(std::holds_alternative<EngineError>(bid_sell));
    ASSERT_TRUE(std::holds_alternative<EngineError>(offer_buy));
    EXPECT_EQ(
        std::get<EngineError>(zero_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(
        std::get<EngineError>(negative_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(
        std::get<EngineError>(bid_sell),
        EngineError::UnsupportedPegCombination
    );
    EXPECT_EQ(
        std::get<EngineError>(offer_buy),
        EngineError::UnsupportedPegCombination
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());

    const SubmissionResult valid_limit = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionReport* report =
        std::get_if<SubmissionReport>(&valid_limit);

    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->order_id, 1);
}

TEST(MatchingEngineTest, RejectsPegWhenReferenceIsUnavailable) {
    MatchingEngine engine;

    const SubmissionResult bid_peg = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        10
    );
    const SubmissionResult offer_peg = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(bid_peg));
    ASSERT_TRUE(std::holds_alternative<EngineError>(offer_peg));
    EXPECT_EQ(
        std::get<EngineError>(bid_peg),
        EngineError::PegReferenceUnavailable
    );
    EXPECT_EQ(
        std::get<EngineError>(offer_peg),
        EngineError::PegReferenceUnavailable
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, AcceptsBidPegAtBestRegularBuyPrice) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 20);
    engine.submit_limit(Side::Buy, 10'50, 30);

    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        15
    );
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(peg_report, nullptr);
    EXPECT_EQ(peg_report->order_id, 3);
    EXPECT_TRUE(peg_report->trades.empty());
    EXPECT_TRUE(peg_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    EXPECT_EQ(peg_order->type(), OrderType::Limit);
    EXPECT_TRUE(peg_order->is_pegged());
    ASSERT_TRUE(peg_order->peg_reference().has_value());
    EXPECT_EQ(peg_order->peg_reference().value(), PegReference::Bid);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'50);
    EXPECT_EQ(peg_order->remaining_quantity(), 15);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_count(), 3);
    EXPECT_EQ(engine.order_book().size(), 3);
    EXPECT_EQ(engine.order_book().find(peg_report->order_id), peg_order);

    const std::optional<Price> reference =
        engine.order_book().best_limit_price(Side::Buy);

    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference.value(), 10'50);
}

TEST(MatchingEngineTest, AcceptsOfferPegAtBestRegularSellPrice) {
    MatchingEngine engine;

    engine.submit_limit(Side::Sell, 11'00, 20);
    engine.submit_limit(Side::Sell, 10'50, 30);

    const SubmissionResult peg_result = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        15
    );
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(peg_report, nullptr);
    EXPECT_EQ(peg_report->order_id, 3);
    EXPECT_TRUE(peg_report->trades.empty());
    EXPECT_TRUE(peg_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    EXPECT_EQ(peg_order->type(), OrderType::Limit);
    EXPECT_TRUE(peg_order->is_pegged());
    ASSERT_TRUE(peg_order->peg_reference().has_value());
    EXPECT_EQ(peg_order->peg_reference().value(), PegReference::Offer);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'50);
    EXPECT_EQ(peg_order->remaining_quantity(), 15);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_count(), 3);
    EXPECT_EQ(engine.order_book().size(), 3);
    EXPECT_EQ(engine.order_book().find(peg_report->order_id), peg_order);

    const std::optional<Price> reference =
        engine.order_book().best_limit_price(Side::Sell);

    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference.value(), 10'50);
}

TEST(MatchingEngineTest, RepricesBidPegAfterBetterBuyLimitRests) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 20);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        15
    );
    const SubmissionResult better_limit_result = engine.submit_limit(
        Side::Buy,
        10'50,
        30
    );

    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* better_limit_report =
        std::get_if<SubmissionReport>(&better_limit_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(better_limit_report, nullptr);
    EXPECT_TRUE(better_limit_report->trades.empty());
    EXPECT_TRUE(better_limit_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);
    const Order* better_limit = engine.find_order(
        better_limit_report->order_id
    );

    ASSERT_NE(peg_order, nullptr);
    ASSERT_NE(better_limit, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'50);
    EXPECT_EQ(peg_order->sequence(), 2);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Buy), peg_order);
}

TEST(MatchingEngineTest, RepricesOfferPegAfterBetterSellLimitRests) {
    MatchingEngine engine;

    engine.submit_limit(Side::Sell, 11'00, 20);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        15
    );
    const SubmissionResult better_limit_result = engine.submit_limit(
        Side::Sell,
        10'50,
        30
    );

    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* better_limit_report =
        std::get_if<SubmissionReport>(&better_limit_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(better_limit_report, nullptr);
    EXPECT_TRUE(better_limit_report->trades.empty());
    EXPECT_TRUE(better_limit_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);
    const Order* better_limit = engine.find_order(
        better_limit_report->order_id
    );

    ASSERT_NE(peg_order, nullptr);
    ASSERT_NE(better_limit, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'50);
    EXPECT_EQ(peg_order->sequence(), 2);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Sell), peg_order);
}

TEST(MatchingEngineTest, PreservesPegPriorityInAssignmentRepricingExample) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 200);
    engine.submit_limit(Side::Buy, 9'99, 100);
    engine.submit_limit(Side::Sell, 10'50, 100);

    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        150
    );
    const SubmissionResult better_limit_result = engine.submit_limit(
        Side::Buy,
        10'10,
        300
    );

    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* better_limit_report =
        std::get_if<SubmissionReport>(&better_limit_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(better_limit_report, nullptr);

    const OrderBookSnapshot snapshot = engine.order_book().snapshot();

    ASSERT_FALSE(snapshot.buys.empty());
    ASSERT_EQ(snapshot.buys.front().price, 10'10);
    ASSERT_EQ(snapshot.buys.front().orders.size(), 2);
    EXPECT_EQ(
        snapshot.buys.front().orders[0].order_id,
        peg_report->order_id
    );
    EXPECT_EQ(snapshot.buys.front().orders[0].remaining_quantity, 150);
    EXPECT_EQ(
        snapshot.buys.front().orders[1].order_id,
        better_limit_report->order_id
    );
    EXPECT_EQ(snapshot.buys.front().orders[1].remaining_quantity, 300);
}

TEST(MatchingEngineTest, KeepsPegPriorityWhenReferencePriceDoesNotChange) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 20);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        15
    );
    const SubmissionResult worse_limit_result = engine.submit_limit(
        Side::Buy,
        9'50,
        30
    );

    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* worse_limit_report =
        std::get_if<SubmissionReport>(&worse_limit_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(worse_limit_report, nullptr);
    EXPECT_TRUE(worse_limit_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'00);
    EXPECT_EQ(peg_order->sequence(), 2);

    const SubmissionResult next_limit_result = engine.submit_limit(
        Side::Buy,
        9'00,
        10
    );
    const SubmissionReport* next_limit_report =
        std::get_if<SubmissionReport>(&next_limit_result);

    ASSERT_NE(next_limit_report, nullptr);

    const Order* next_limit = engine.find_order(
        next_limit_report->order_id
    );

    ASSERT_NE(next_limit, nullptr);
    EXPECT_EQ(next_limit->sequence(), 4);
}

TEST(MatchingEngineTest, RepricesMultiplePegsInPreviousPriorityOrder) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult first_peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        10
    );
    const SubmissionResult second_peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        10
    );
    engine.submit_limit(Side::Buy, 10'50, 10);

    const SubmissionReport* first_peg_report =
        std::get_if<SubmissionReport>(&first_peg_result);
    const SubmissionReport* second_peg_report =
        std::get_if<SubmissionReport>(&second_peg_result);

    ASSERT_NE(first_peg_report, nullptr);
    ASSERT_NE(second_peg_report, nullptr);

    const Order* first_peg = engine.find_order(
        first_peg_report->order_id
    );
    const Order* second_peg = engine.find_order(
        second_peg_report->order_id
    );

    ASSERT_NE(first_peg, nullptr);
    ASSERT_NE(second_peg, nullptr);
    ASSERT_TRUE(first_peg->price().has_value());
    ASSERT_TRUE(second_peg->price().has_value());
    EXPECT_EQ(first_peg->price().value(), 10'50);
    EXPECT_EQ(second_peg->price().value(), 10'50);
    EXPECT_EQ(first_peg->sequence(), 2);
    EXPECT_EQ(second_peg->sequence(), 3);
}

TEST(MatchingEngineTest, RefreshesPegOnlyAfterMarketFinishesMatching) {
    MatchingEngine engine;

    const SubmissionResult best_limit_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionResult worse_limit_result = engine.submit_limit(
        Side::Buy,
        9'00,
        20
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Sell,
        15
    );

    const SubmissionReport* best_limit_report =
        std::get_if<SubmissionReport>(&best_limit_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* worse_limit_report =
        std::get_if<SubmissionReport>(&worse_limit_result);
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(best_limit_report, nullptr);
    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(worse_limit_report, nullptr);
    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 2);
    EXPECT_EQ(
        market_report->trades[0].resting_order_id,
        best_limit_report->order_id
    );
    EXPECT_EQ(market_report->trades[0].price, 10'00);
    EXPECT_EQ(market_report->trades[0].quantity, 10);
    EXPECT_EQ(
        market_report->trades[1].resting_order_id,
        peg_report->order_id
    );
    EXPECT_EQ(market_report->trades[1].price, 10'00);
    EXPECT_EQ(market_report->trades[1].quantity, 5);
    EXPECT_TRUE(market_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);
    const Order* worse_limit = engine.find_order(
        worse_limit_report->order_id
    );

    ASSERT_NE(peg_order, nullptr);
    ASSERT_NE(worse_limit, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 9'00);
    EXPECT_EQ(peg_order->remaining_quantity(), 15);
    EXPECT_EQ(peg_order->sequence(), 2);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Buy), peg_order);
}

TEST(MatchingEngineTest, CancelsAllPegsWhenLastReferenceDisappears) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult first_peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionResult second_peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        30
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Sell,
        10
    );

    const SubmissionReport* first_peg_report =
        std::get_if<SubmissionReport>(&first_peg_result);
    const SubmissionReport* second_peg_report =
        std::get_if<SubmissionReport>(&second_peg_result);
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(first_peg_report, nullptr);
    ASSERT_NE(second_peg_report, nullptr);
    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 1);
    ASSERT_EQ(market_report->cancellations.size(), 2);
    EXPECT_EQ(
        market_report->cancellations[0].order_id,
        first_peg_report->order_id
    );
    EXPECT_EQ(
        market_report->cancellations[1].order_id,
        second_peg_report->order_id
    );

    const Order* first_peg = engine.find_order(
        first_peg_report->order_id
    );
    const Order* second_peg = engine.find_order(
        second_peg_report->order_id
    );

    ASSERT_NE(first_peg, nullptr);
    ASSERT_NE(second_peg, nullptr);
    EXPECT_EQ(first_peg->status(), OrderStatus::Cancelled);
    EXPECT_EQ(first_peg->remaining_quantity(), 0);
    EXPECT_EQ(second_peg->status(), OrderStatus::Cancelled);
    EXPECT_EQ(second_peg->remaining_quantity(), 0);
    EXPECT_TRUE(engine.order_book().empty());

    const SubmissionResult new_reference_result = engine.submit_limit(
        Side::Buy,
        9'00,
        10
    );
    const SubmissionReport* new_reference_report =
        std::get_if<SubmissionReport>(&new_reference_result);

    ASSERT_NE(new_reference_report, nullptr);
    EXPECT_TRUE(new_reference_report->cancellations.empty());
    EXPECT_EQ(engine.order_book().size(), 1);
    EXPECT_EQ(first_peg->status(), OrderStatus::Cancelled);
    EXPECT_EQ(second_peg->status(), OrderStatus::Cancelled);
}

TEST(MatchingEngineTest, LimitSubmissionCancelsPegAfterConsumingReference) {
    MatchingEngine engine;

    const SubmissionResult reference_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        10
    );

    const SubmissionReport* reference_report =
        std::get_if<SubmissionReport>(&reference_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);

    ASSERT_NE(reference_report, nullptr);
    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(sell_report, nullptr);
    ASSERT_EQ(sell_report->trades.size(), 1);
    EXPECT_EQ(
        sell_report->trades.front().resting_order_id,
        reference_report->order_id
    );
    ASSERT_EQ(sell_report->cancellations.size(), 1);
    EXPECT_EQ(
        sell_report->cancellations.front().order_id,
        peg_report->order_id
    );

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    EXPECT_EQ(peg_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(peg_order->remaining_quantity(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RemovesFilledPegFromPegRegistry) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        10
    );
    const SubmissionResult market_result = engine.submit_market(
        Side::Sell,
        20
    );

    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 2);
    EXPECT_TRUE(market_report->cancellations.empty());

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    EXPECT_EQ(peg_order->status(), OrderStatus::Filled);
    EXPECT_EQ(peg_order->remaining_quantity(), 0);
    EXPECT_TRUE(engine.order_book().empty());

    const SubmissionResult new_reference_result = engine.submit_limit(
        Side::Buy,
        9'00,
        10
    );
    const SubmissionReport* new_reference_report =
        std::get_if<SubmissionReport>(&new_reference_result);

    ASSERT_NE(new_reference_report, nullptr);
    EXPECT_TRUE(new_reference_report->cancellations.empty());
    EXPECT_EQ(peg_order->status(), OrderStatus::Filled);
    EXPECT_EQ(engine.order_book().size(), 1);
}

TEST(MatchingEngineTest, RejectsCancellationForUnknownOrder) {
    MatchingEngine engine;

    const CancellationResult result = engine.cancel_order(999);

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::OrderNotFound
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, CancelsActiveLimitOrder) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        25
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);

    const CancellationResult cancellation = engine.cancel_order(
        submission_report->order_id
    );
    const CancellationReport* cancellation_report =
        std::get_if<CancellationReport>(&cancellation);

    ASSERT_NE(cancellation_report, nullptr);
    EXPECT_EQ(
        cancellation_report->order_id,
        submission_report->order_id
    );
    ASSERT_EQ(cancellation_report->cancellations.size(), 1);
    EXPECT_EQ(
        cancellation_report->cancellations.front().order_id,
        submission_report->order_id
    );
    EXPECT_EQ(cancellation_report->cancellations.front().quantity, 25);
    EXPECT_EQ(
        cancellation_report->cancellations.front().reason,
        CancellationReason::UserRequested
    );

    const Order* order = engine.find_order(submission_report->order_id);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(order->remaining_quantity(), 0);
    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RejectsRepeatedCancellation) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Sell,
        10'00,
        10
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);
    ASSERT_TRUE(std::holds_alternative<CancellationReport>(
        engine.cancel_order(submission_report->order_id)
    ));

    const CancellationResult second_cancellation = engine.cancel_order(
        submission_report->order_id
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(
        second_cancellation
    ));
    EXPECT_EQ(
        std::get<EngineError>(second_cancellation),
        EngineError::OrderNotOpen
    );
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RejectsCancellationForFilledOrder) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        10
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );

    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);
    ASSERT_EQ(buy_report->trades.size(), 1);

    const CancellationResult cancellation = engine.cancel_order(
        sell_report->order_id
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(cancellation));
    EXPECT_EQ(
        std::get<EngineError>(cancellation),
        EngineError::OrderNotOpen
    );
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, CancelsPegWithoutReactivatingIt) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(peg_report, nullptr);

    const CancellationResult cancellation = engine.cancel_order(
        peg_report->order_id
    );
    const CancellationReport* cancellation_report =
        std::get_if<CancellationReport>(&cancellation);

    ASSERT_NE(cancellation_report, nullptr);
    ASSERT_EQ(cancellation_report->cancellations.size(), 1);
    EXPECT_EQ(
        cancellation_report->cancellations.front().order_id,
        peg_report->order_id
    );

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    EXPECT_EQ(peg_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(peg_order->remaining_quantity(), 0);
    EXPECT_EQ(engine.order_book().size(), 1);

    const SubmissionResult better_reference_result = engine.submit_limit(
        Side::Buy,
        10'50,
        10
    );
    const SubmissionReport* better_reference_report =
        std::get_if<SubmissionReport>(&better_reference_result);

    ASSERT_NE(better_reference_report, nullptr);
    EXPECT_TRUE(better_reference_report->cancellations.empty());
    EXPECT_EQ(peg_order->status(), OrderStatus::Cancelled);
    EXPECT_EQ(engine.order_book().size(), 2);
}

TEST(MatchingEngineTest, RepricesPegAfterBestReferenceIsCancelled) {
    MatchingEngine engine;

    const SubmissionResult worse_reference_result = engine.submit_limit(
        Side::Buy,
        9'00,
        10
    );
    const SubmissionResult best_reference_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );

    const SubmissionReport* worse_reference_report =
        std::get_if<SubmissionReport>(&worse_reference_result);
    const SubmissionReport* best_reference_report =
        std::get_if<SubmissionReport>(&best_reference_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(worse_reference_report, nullptr);
    ASSERT_NE(best_reference_report, nullptr);
    ASSERT_NE(peg_report, nullptr);

    const CancellationResult cancellation = engine.cancel_order(
        best_reference_report->order_id
    );
    const CancellationReport* cancellation_report =
        std::get_if<CancellationReport>(&cancellation);

    ASSERT_NE(cancellation_report, nullptr);
    ASSERT_EQ(cancellation_report->cancellations.size(), 1);
    EXPECT_EQ(
        cancellation_report->cancellations.front().order_id,
        best_reference_report->order_id
    );

    const Order* peg_order = engine.find_order(peg_report->order_id);
    const Order* worse_reference = engine.find_order(
        worse_reference_report->order_id
    );

    ASSERT_NE(peg_order, nullptr);
    ASSERT_NE(worse_reference, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 9'00);
    EXPECT_EQ(peg_order->sequence(), 3);
    EXPECT_EQ(peg_order->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Buy), worse_reference);
}

TEST(MatchingEngineTest, CancelsDependentPegsWithLastReference) {
    MatchingEngine engine;

    const SubmissionResult reference_result = engine.submit_limit(
        Side::Sell,
        10'00,
        10
    );
    const SubmissionResult first_peg_result = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        20
    );
    const SubmissionResult second_peg_result = engine.submit_peg(
        Side::Sell,
        PegReference::Offer,
        30
    );

    const SubmissionReport* reference_report =
        std::get_if<SubmissionReport>(&reference_result);
    const SubmissionReport* first_peg_report =
        std::get_if<SubmissionReport>(&first_peg_result);
    const SubmissionReport* second_peg_report =
        std::get_if<SubmissionReport>(&second_peg_result);

    ASSERT_NE(reference_report, nullptr);
    ASSERT_NE(first_peg_report, nullptr);
    ASSERT_NE(second_peg_report, nullptr);

    const CancellationResult cancellation = engine.cancel_order(
        reference_report->order_id
    );
    const CancellationReport* cancellation_report =
        std::get_if<CancellationReport>(&cancellation);

    ASSERT_NE(cancellation_report, nullptr);
    ASSERT_EQ(cancellation_report->cancellations.size(), 3);
    EXPECT_EQ(
        cancellation_report->cancellations[0].order_id,
        reference_report->order_id
    );
    EXPECT_EQ(
        cancellation_report->cancellations[1].order_id,
        first_peg_report->order_id
    );
    EXPECT_EQ(
        cancellation_report->cancellations[2].order_id,
        second_peg_report->order_id
    );
    EXPECT_EQ(
        cancellation_report->cancellations[0].reason,
        CancellationReason::UserRequested
    );
    EXPECT_EQ(
        cancellation_report->cancellations[1].reason,
        CancellationReason::PegReferenceUnavailable
    );
    EXPECT_EQ(
        cancellation_report->cancellations[2].reason,
        CancellationReason::PegReferenceUnavailable
    );

    const Order* reference = engine.find_order(
        reference_report->order_id
    );
    const Order* first_peg = engine.find_order(
        first_peg_report->order_id
    );
    const Order* second_peg = engine.find_order(
        second_peg_report->order_id
    );

    ASSERT_NE(reference, nullptr);
    ASSERT_NE(first_peg, nullptr);
    ASSERT_NE(second_peg, nullptr);
    EXPECT_EQ(reference->status(), OrderStatus::Cancelled);
    EXPECT_EQ(first_peg->status(), OrderStatus::Cancelled);
    EXPECT_EQ(second_peg->status(), OrderStatus::Cancelled);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, KeepsPegPriorityAfterUnrelatedCancellation) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult worse_reference_result = engine.submit_limit(
        Side::Buy,
        9'00,
        10
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );

    const SubmissionReport* worse_reference_report =
        std::get_if<SubmissionReport>(&worse_reference_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(worse_reference_report, nullptr);
    ASSERT_NE(peg_report, nullptr);

    const CancellationResult cancellation = engine.cancel_order(
        worse_reference_report->order_id
    );
    const CancellationReport* cancellation_report =
        std::get_if<CancellationReport>(&cancellation);

    ASSERT_NE(cancellation_report, nullptr);
    ASSERT_EQ(cancellation_report->cancellations.size(), 1);

    const Order* peg_order = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg_order, nullptr);
    ASSERT_TRUE(peg_order->price().has_value());
    EXPECT_EQ(peg_order->price().value(), 10'00);
    EXPECT_EQ(peg_order->sequence(), 3);

    const SubmissionResult next_limit_result = engine.submit_limit(
        Side::Buy,
        8'00,
        10
    );
    const SubmissionReport* next_limit_report =
        std::get_if<SubmissionReport>(&next_limit_result);

    ASSERT_NE(next_limit_report, nullptr);

    const Order* next_limit = engine.find_order(
        next_limit_report->order_id
    );

    ASSERT_NE(next_limit, nullptr);
    EXPECT_EQ(next_limit->sequence(), 4);
}

TEST(MatchingEngineTest, RejectsInvalidQuantityAmendment) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);

    const AmendmentResult zero_quantity = engine.amend_quantity(
        submission_report->order_id,
        0
    );
    const AmendmentResult negative_quantity = engine.amend_quantity(
        submission_report->order_id,
        -10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_quantity));
    ASSERT_TRUE(std::holds_alternative<EngineError>(negative_quantity));
    EXPECT_EQ(
        std::get<EngineError>(zero_quantity),
        EngineError::InvalidQuantity
    );
    EXPECT_EQ(
        std::get<EngineError>(negative_quantity),
        EngineError::InvalidQuantity
    );

    const Order* order = engine.find_order(submission_report->order_id);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remaining_quantity(), 20);
    EXPECT_EQ(order->sequence(), 1);
    EXPECT_EQ(order->status(), OrderStatus::Active);
}

TEST(MatchingEngineTest, RejectsQuantityAmendmentForUnknownOrder) {
    MatchingEngine engine;

    const AmendmentResult result = engine.amend_quantity(999, 10);

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::OrderNotFound
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RejectsQuantityAmendmentForClosedOrder) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);
    ASSERT_TRUE(std::holds_alternative<CancellationReport>(
        engine.cancel_order(submission_report->order_id)
    ));

    const AmendmentResult result = engine.amend_quantity(
        submission_report->order_id,
        10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::OrderNotOpen
    );
}

TEST(MatchingEngineTest, ReducesQuantityWithoutLosingPriority) {
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

    const SubmissionReport* first_sell_report =
        std::get_if<SubmissionReport>(&first_sell_result);
    const SubmissionReport* second_sell_report =
        std::get_if<SubmissionReport>(&second_sell_result);

    ASSERT_NE(first_sell_report, nullptr);
    ASSERT_NE(second_sell_report, nullptr);

    const AmendmentResult amendment = engine.amend_quantity(
        first_sell_report->order_id,
        10
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    EXPECT_EQ(amendment_report->order_id, first_sell_report->order_id);
    EXPECT_TRUE(amendment_report->trades.empty());
    EXPECT_TRUE(amendment_report->cancellations.empty());

    const Order* first_sell = engine.find_order(
        first_sell_report->order_id
    );

    ASSERT_NE(first_sell, nullptr);
    EXPECT_EQ(first_sell->original_quantity(), 20);
    EXPECT_EQ(first_sell->remaining_quantity(), 10);
    EXPECT_EQ(first_sell->sequence(), 1);

    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        15
    );
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 2);
    EXPECT_EQ(
        market_report->trades[0].resting_order_id,
        first_sell_report->order_id
    );
    EXPECT_EQ(market_report->trades[0].quantity, 10);
    EXPECT_EQ(
        market_report->trades[1].resting_order_id,
        second_sell_report->order_id
    );
    EXPECT_EQ(market_report->trades[1].quantity, 5);
}

TEST(MatchingEngineTest, IncreasesQuantityAndLosesPriority) {
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

    const SubmissionReport* first_sell_report =
        std::get_if<SubmissionReport>(&first_sell_result);
    const SubmissionReport* second_sell_report =
        std::get_if<SubmissionReport>(&second_sell_result);

    ASSERT_NE(first_sell_report, nullptr);
    ASSERT_NE(second_sell_report, nullptr);

    const AmendmentResult amendment = engine.amend_quantity(
        first_sell_report->order_id,
        30
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    EXPECT_TRUE(amendment_report->trades.empty());
    EXPECT_TRUE(amendment_report->cancellations.empty());

    const Order* first_sell = engine.find_order(
        first_sell_report->order_id
    );

    ASSERT_NE(first_sell, nullptr);
    EXPECT_EQ(first_sell->original_quantity(), 20);
    EXPECT_EQ(first_sell->remaining_quantity(), 30);
    EXPECT_EQ(first_sell->sequence(), 3);

    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        25
    );
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 2);
    EXPECT_EQ(
        market_report->trades[0].resting_order_id,
        second_sell_report->order_id
    );
    EXPECT_EQ(market_report->trades[0].quantity, 20);
    EXPECT_EQ(
        market_report->trades[1].resting_order_id,
        first_sell_report->order_id
    );
    EXPECT_EQ(market_report->trades[1].quantity, 5);
}

TEST(MatchingEngineTest, KeepsPriorityForUnchangedQuantity) {
    MatchingEngine engine;

    const SubmissionResult first_limit_result = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* first_limit_report =
        std::get_if<SubmissionReport>(&first_limit_result);

    ASSERT_NE(first_limit_report, nullptr);

    const AmendmentResult amendment = engine.amend_quantity(
        first_limit_report->order_id,
        20
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);

    const SubmissionResult second_limit_result = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* second_limit_report =
        std::get_if<SubmissionReport>(&second_limit_result);

    ASSERT_NE(second_limit_report, nullptr);

    const Order* first_limit = engine.find_order(
        first_limit_report->order_id
    );
    const Order* second_limit = engine.find_order(
        second_limit_report->order_id
    );

    ASSERT_NE(first_limit, nullptr);
    ASSERT_NE(second_limit, nullptr);
    EXPECT_EQ(first_limit->sequence(), 1);
    EXPECT_EQ(second_limit->sequence(), 2);
    EXPECT_EQ(engine.order_book().best(Side::Buy), first_limit);
}

TEST(MatchingEngineTest, AmendsRemainingQuantityAfterPartialFill) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    engine.submit_market(Side::Buy, 5);

    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);

    ASSERT_NE(sell_report, nullptr);

    const Order* sell_before_amendment = engine.find_order(
        sell_report->order_id
    );

    ASSERT_NE(sell_before_amendment, nullptr);
    EXPECT_EQ(sell_before_amendment->remaining_quantity(), 15);

    const AmendmentResult amendment = engine.amend_quantity(
        sell_report->order_id,
        8
    );

    ASSERT_TRUE(std::holds_alternative<AmendmentReport>(amendment));

    const Order* sell_after_amendment = engine.find_order(
        sell_report->order_id
    );

    ASSERT_NE(sell_after_amendment, nullptr);
    EXPECT_EQ(sell_after_amendment->original_quantity(), 20);
    EXPECT_EQ(sell_after_amendment->remaining_quantity(), 8);
    EXPECT_EQ(sell_after_amendment->sequence(), 1);
}

TEST(MatchingEngineTest, AmendsPegQuantityWithoutLosingRegistration) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 10);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(peg_report, nullptr);
    ASSERT_TRUE(std::holds_alternative<AmendmentReport>(
        engine.amend_quantity(peg_report->order_id, 10)
    ));
    ASSERT_TRUE(std::holds_alternative<AmendmentReport>(
        engine.amend_quantity(peg_report->order_id, 30)
    ));

    const Order* peg_before_repricing = engine.find_order(
        peg_report->order_id
    );

    ASSERT_NE(peg_before_repricing, nullptr);
    EXPECT_EQ(peg_before_repricing->remaining_quantity(), 30);
    EXPECT_EQ(peg_before_repricing->sequence(), 3);

    engine.submit_limit(Side::Buy, 10'50, 10);

    const Order* peg_after_repricing = engine.find_order(
        peg_report->order_id
    );

    ASSERT_NE(peg_after_repricing, nullptr);
    ASSERT_TRUE(peg_after_repricing->price().has_value());
    EXPECT_EQ(peg_after_repricing->price().value(), 10'50);
    EXPECT_EQ(peg_after_repricing->remaining_quantity(), 30);
    EXPECT_EQ(peg_after_repricing->sequence(), 3);
    EXPECT_EQ(peg_after_repricing->status(), OrderStatus::Active);
}

TEST(MatchingEngineTest, RejectsInvalidPriceAmendment) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);

    const AmendmentResult zero_price = engine.amend_price(
        submission_report->order_id,
        0
    );
    const AmendmentResult negative_price = engine.amend_price(
        submission_report->order_id,
        -10
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(zero_price));
    ASSERT_TRUE(std::holds_alternative<EngineError>(negative_price));
    EXPECT_EQ(
        std::get<EngineError>(zero_price),
        EngineError::InvalidPrice
    );
    EXPECT_EQ(
        std::get<EngineError>(negative_price),
        EngineError::InvalidPrice
    );

    const Order* order = engine.find_order(submission_report->order_id);

    ASSERT_NE(order, nullptr);
    ASSERT_TRUE(order->price().has_value());
    EXPECT_EQ(order->price().value(), 10'00);
    EXPECT_EQ(order->sequence(), 1);
}

TEST(MatchingEngineTest, RejectsPriceAmendmentForUnknownOrder) {
    MatchingEngine engine;

    const AmendmentResult result = engine.amend_price(999, 10'00);

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::OrderNotFound
    );
    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RejectsPriceAmendmentForClosedOrder) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* submission_report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(submission_report, nullptr);
    ASSERT_TRUE(std::holds_alternative<CancellationReport>(
        engine.cancel_order(submission_report->order_id)
    ));

    const AmendmentResult result = engine.amend_price(
        submission_report->order_id,
        10'25
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(result));
    EXPECT_EQ(
        std::get<EngineError>(result),
        EngineError::OrderNotOpen
    );
}

TEST(MatchingEngineTest, RejectsManualPriceAmendmentForPeg) {
    MatchingEngine engine;

    engine.submit_limit(Side::Buy, 10'00, 20);
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(peg_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        peg_report->order_id,
        10'25
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(amendment));
    EXPECT_EQ(
        std::get<EngineError>(amendment),
        EngineError::UnsupportedAmendment
    );

    const Order* peg = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg, nullptr);
    ASSERT_TRUE(peg->price().has_value());
    EXPECT_EQ(peg->price().value(), 10'00);
    EXPECT_EQ(peg->sequence(), 2);
    EXPECT_EQ(peg->status(), OrderStatus::Active);
}

TEST(MatchingEngineTest, KeepsPriorityForUnchangedPrice) {
    MatchingEngine engine;

    const SubmissionResult first_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionReport* first_report =
        std::get_if<SubmissionReport>(&first_result);

    ASSERT_NE(first_report, nullptr);
    ASSERT_TRUE(std::holds_alternative<AmendmentReport>(
        engine.amend_price(first_report->order_id, 10'00)
    ));

    const SubmissionResult second_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionReport* second_report =
        std::get_if<SubmissionReport>(&second_result);

    ASSERT_NE(second_report, nullptr);

    const Order* first = engine.find_order(first_report->order_id);
    const Order* second = engine.find_order(second_report->order_id);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->sequence(), 1);
    EXPECT_EQ(second->sequence(), 2);
    EXPECT_EQ(engine.order_book().best(Side::Sell), first);
}

TEST(MatchingEngineTest, PriceChangeLosesPriorityAtNewLevel) {
    MatchingEngine engine;

    const SubmissionResult amended_result = engine.submit_limit(
        Side::Sell,
        10'50,
        20
    );
    const SubmissionResult resting_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionReport* amended_report =
        std::get_if<SubmissionReport>(&amended_result);
    const SubmissionReport* resting_report =
        std::get_if<SubmissionReport>(&resting_result);

    ASSERT_NE(amended_report, nullptr);
    ASSERT_NE(resting_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        amended_report->order_id,
        10'00
    );

    ASSERT_TRUE(std::holds_alternative<AmendmentReport>(amendment));

    const Order* amended = engine.find_order(amended_report->order_id);

    ASSERT_NE(amended, nullptr);
    EXPECT_EQ(amended->sequence(), 3);

    const SubmissionResult market_result = engine.submit_market(
        Side::Buy,
        25
    );
    const SubmissionReport* market_report =
        std::get_if<SubmissionReport>(&market_result);

    ASSERT_NE(market_report, nullptr);
    ASSERT_EQ(market_report->trades.size(), 2);
    EXPECT_EQ(
        market_report->trades[0].resting_order_id,
        resting_report->order_id
    );
    EXPECT_EQ(market_report->trades[0].quantity, 20);
    EXPECT_EQ(
        market_report->trades[1].resting_order_id,
        amended_report->order_id
    );
    EXPECT_EQ(market_report->trades[1].quantity, 5);
}

TEST(MatchingEngineTest, PriceAmendmentCanCrossAndFillOrder) {
    MatchingEngine engine;

    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        9'50,
        20
    );
    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        20
    );
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);
    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);

    ASSERT_NE(buy_report, nullptr);
    ASSERT_NE(sell_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        buy_report->order_id,
        10'00
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    ASSERT_EQ(amendment_report->trades.size(), 1);
    EXPECT_EQ(
        amendment_report->trades[0].resting_order_id,
        sell_report->order_id
    );
    EXPECT_EQ(
        amendment_report->trades[0].aggressive_order_id,
        buy_report->order_id
    );
    EXPECT_EQ(amendment_report->trades[0].price, 10'00);
    EXPECT_EQ(amendment_report->trades[0].quantity, 20);

    const Order* buy = engine.find_order(buy_report->order_id);
    const Order* sell = engine.find_order(sell_report->order_id);

    ASSERT_NE(buy, nullptr);
    ASSERT_NE(sell, nullptr);
    EXPECT_EQ(buy->status(), OrderStatus::Filled);
    EXPECT_EQ(sell->status(), OrderStatus::Filled);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RestsRemainderAfterCrossingPriceAmendment) {
    MatchingEngine engine;

    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        9'50,
        30
    );
    engine.submit_limit(Side::Sell, 10'00, 10);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(buy_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        buy_report->order_id,
        10'00
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    ASSERT_EQ(amendment_report->trades.size(), 1);
    EXPECT_EQ(amendment_report->trades[0].price, 10'00);
    EXPECT_EQ(amendment_report->trades[0].quantity, 10);

    const Order* buy = engine.find_order(buy_report->order_id);

    ASSERT_NE(buy, nullptr);
    ASSERT_TRUE(buy->price().has_value());
    EXPECT_EQ(buy->price().value(), 10'00);
    EXPECT_EQ(buy->remaining_quantity(), 20);
    EXPECT_EQ(buy->sequence(), 3);
    EXPECT_EQ(buy->status(), OrderStatus::Active);
    EXPECT_EQ(engine.order_book().best(Side::Buy), buy);
}

TEST(MatchingEngineTest, PriceAmendmentRefreshesPegAfterMatching) {
    MatchingEngine engine;

    const SubmissionResult first_bid_result = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    const SubmissionResult second_bid_result = engine.submit_limit(
        Side::Buy,
        9'50,
        20
    );
    const SubmissionReport* first_bid_report =
        std::get_if<SubmissionReport>(&first_bid_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);
    const SubmissionReport* second_bid_report =
        std::get_if<SubmissionReport>(&second_bid_result);

    ASSERT_NE(first_bid_report, nullptr);
    ASSERT_NE(peg_report, nullptr);
    ASSERT_NE(second_bid_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        first_bid_report->order_id,
        9'25
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    EXPECT_TRUE(amendment_report->trades.empty());
    EXPECT_TRUE(amendment_report->cancellations.empty());

    const Order* peg = engine.find_order(peg_report->order_id);

    ASSERT_NE(peg, nullptr);
    ASSERT_TRUE(peg->price().has_value());
    EXPECT_EQ(peg->price().value(), 9'50);
    EXPECT_EQ(peg->sequence(), 2);
    EXPECT_EQ(peg->status(), OrderStatus::Active);
}

TEST(MatchingEngineTest, PriceAmendmentCancelsPegWithoutReference) {
    MatchingEngine engine;

    const SubmissionResult bid_result = engine.submit_limit(
        Side::Buy,
        10'00,
        10
    );
    const SubmissionResult peg_result = engine.submit_peg(
        Side::Buy,
        PegReference::Bid,
        20
    );
    engine.submit_limit(Side::Sell, 10'50, 10);

    const SubmissionReport* bid_report =
        std::get_if<SubmissionReport>(&bid_result);
    const SubmissionReport* peg_report =
        std::get_if<SubmissionReport>(&peg_result);

    ASSERT_NE(bid_report, nullptr);
    ASSERT_NE(peg_report, nullptr);

    const AmendmentResult amendment = engine.amend_price(
        bid_report->order_id,
        10'50
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    ASSERT_EQ(amendment_report->trades.size(), 1);
    ASSERT_EQ(amendment_report->cancellations.size(), 1);
    EXPECT_EQ(
        amendment_report->cancellations[0].order_id,
        peg_report->order_id
    );
    EXPECT_EQ(amendment_report->cancellations[0].quantity, 20);
    EXPECT_EQ(
        amendment_report->cancellations[0].reason,
        CancellationReason::PegReferenceUnavailable
    );

    const Order* bid = engine.find_order(bid_report->order_id);
    const Order* peg = engine.find_order(peg_report->order_id);

    ASSERT_NE(bid, nullptr);
    ASSERT_NE(peg, nullptr);
    EXPECT_EQ(bid->status(), OrderStatus::Filled);
    EXPECT_EQ(peg->status(), OrderStatus::Cancelled);
    EXPECT_EQ(peg->remaining_quantity(), 0);
    EXPECT_TRUE(engine.order_book().empty());
}

TEST(MatchingEngineTest, RejectsEmptyAmendment) {
    MatchingEngine engine;

    const SubmissionResult submission = engine.submit_limit(
        Side::Buy,
        10'00,
        20
    );
    const SubmissionReport* report =
        std::get_if<SubmissionReport>(&submission);

    ASSERT_NE(report, nullptr);

    const AmendmentResult amendment = engine.amend_order(
        report->order_id,
        AmendmentRequest{
            std::nullopt,
            std::nullopt
        }
    );

    ASSERT_TRUE(std::holds_alternative<EngineError>(amendment));
    EXPECT_EQ(
        std::get<EngineError>(amendment),
        EngineError::EmptyAmendment
    );
}

TEST(MatchingEngineTest, AppliesPriceAndQuantityBeforeMatching) {
    MatchingEngine engine;

    const SubmissionResult sell_result = engine.submit_limit(
        Side::Sell,
        10'00,
        150
    );
    const SubmissionResult buy_result = engine.submit_limit(
        Side::Buy,
        9'00,
        100
    );
    const SubmissionReport* sell_report =
        std::get_if<SubmissionReport>(&sell_result);
    const SubmissionReport* buy_report =
        std::get_if<SubmissionReport>(&buy_result);

    ASSERT_NE(sell_report, nullptr);
    ASSERT_NE(buy_report, nullptr);

    const AmendmentResult amendment = engine.amend_order(
        buy_report->order_id,
        AmendmentRequest{
            10'00,
            200
        }
    );
    const AmendmentReport* amendment_report =
        std::get_if<AmendmentReport>(&amendment);

    ASSERT_NE(amendment_report, nullptr);
    ASSERT_EQ(amendment_report->trades.size(), 1);
    EXPECT_EQ(
        amendment_report->trades.front().resting_order_id,
        sell_report->order_id
    );
    EXPECT_EQ(amendment_report->trades.front().price, 10'00);
    EXPECT_EQ(amendment_report->trades.front().quantity, 150);

    const Order* buy = engine.find_order(buy_report->order_id);
    const Order* sell = engine.find_order(sell_report->order_id);

    ASSERT_NE(buy, nullptr);
    ASSERT_NE(sell, nullptr);
    ASSERT_TRUE(buy->price().has_value());
    EXPECT_EQ(buy->price().value(), 10'00);
    EXPECT_EQ(buy->remaining_quantity(), 50);
    EXPECT_EQ(buy->original_quantity(), 100);
    EXPECT_EQ(buy->sequence(), 3);
    EXPECT_EQ(buy->status(), OrderStatus::Active);
    EXPECT_EQ(sell->status(), OrderStatus::Filled);
    EXPECT_EQ(engine.order_book().best(Side::Buy), buy);
}

}  // namespace
}  // namespace matching_engine
