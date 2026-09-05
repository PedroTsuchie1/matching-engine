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
    ASSERT_EQ(report->cancelled_order_ids.size(), 1);
    EXPECT_EQ(report->cancelled_order_ids.front(), report->order_id);

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
    EXPECT_TRUE(market->cancelled_order_ids.empty());

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
    ASSERT_EQ(market->cancelled_order_ids.size(), 1);
    EXPECT_EQ(market->cancelled_order_ids.front(), market->order_id);

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
    EXPECT_TRUE(market->cancelled_order_ids.empty());

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
    EXPECT_TRUE(market->cancelled_order_ids.empty());

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
    ASSERT_EQ(market->cancelled_order_ids.size(), 1);
    EXPECT_EQ(market->cancelled_order_ids.front(), market->order_id);

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
    EXPECT_TRUE(peg_report->cancelled_order_ids.empty());

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
    EXPECT_TRUE(peg_report->cancelled_order_ids.empty());

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

}  // namespace
}  // namespace matching_engine
