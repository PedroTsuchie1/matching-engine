#include "matching_engine/formatting.hpp"

#include <gtest/gtest.h>

#include <string>

namespace matching_engine {
namespace {

OrderBookSnapshot sample_snapshot() {
    return OrderBookSnapshot{
        {
            BookLevelSnapshot{
                10'50,
                50,
                {
                    BookOrderSnapshot{3, 30, 2, std::nullopt},
                    BookOrderSnapshot{
                        2, 20, 4, PegReference::Bid
                    }
                }
            },
            BookLevelSnapshot{
                10'00,
                10,
                {
                    BookOrderSnapshot{1, 10, 1, std::nullopt}
                }
            }
        },
        {
            BookLevelSnapshot{
                10'75,
                40,
                {
                    BookOrderSnapshot{
                        4, 40, 3, PegReference::Offer
                    }
                }
            },
            BookLevelSnapshot{
                11'00,
                30,
                {
                    BookOrderSnapshot{5, 30, 5, std::nullopt}
                }
            }
        }
    };
}

TEST(FormattingTest, FormatsFixedPointPrices) {
    EXPECT_EQ(format_price(1), "0.01");
    EXPECT_EQ(format_price(1'05), "1.05");
    EXPECT_EQ(format_price(9'99), "9.99");
    EXPECT_EQ(format_price(10'00), "10");
    EXPECT_EQ(format_price(10'50), "10.5");
    EXPECT_EQ(format_price(20'00), "20");
}

TEST(FormattingTest, FormatsEmptyBookInBothModes) {
    const OrderBookSnapshot snapshot;

    const std::string summary = format_book_summary(snapshot);
    const std::string detailed = format_book_detailed(snapshot);

    EXPECT_LT(summary.find("BUY"), summary.find("SELL"));
    EXPECT_NE(summary.find("<empty>"), std::string::npos);
    EXPECT_LT(detailed.find("BUY"), detailed.find("SELL"));
    EXPECT_NE(detailed.find("<empty>"), std::string::npos);
}

TEST(FormattingTest, FormatsSummarySideBySideByDepth) {
    const std::string output = format_book_summary(sample_snapshot());

    const std::size_t best_buy = output.find("10.5");
    const std::size_t best_sell = output.find("10.75");
    const std::size_t second_buy = output.find("10 ");
    const std::size_t second_sell = output.find("11 ");

    ASSERT_NE(best_buy, std::string::npos);
    ASSERT_NE(best_sell, std::string::npos);
    ASSERT_NE(second_buy, std::string::npos);
    ASSERT_NE(second_sell, std::string::npos);

    EXPECT_EQ(
        output.substr(0, best_buy).find_last_of('\n'),
        output.substr(0, best_sell).find_last_of('\n')
    );
    EXPECT_EQ(
        output.substr(0, second_buy).find_last_of('\n'),
        output.substr(0, second_sell).find_last_of('\n')
    );
    EXPECT_LT(best_buy, second_buy);
    EXPECT_LT(best_sell, second_sell);
}

TEST(FormattingTest, FormatsDetailedBookWithOrderPriority) {
    const std::string output = format_book_detailed(sample_snapshot());

    const std::size_t first_order = output.find("id=3");
    const std::size_t second_order = output.find("id=2");

    ASSERT_NE(first_order, std::string::npos);
    ASSERT_NE(second_order, std::string::npos);
    EXPECT_LT(first_order, second_order);
    EXPECT_NE(output.find("type=LIMIT"), std::string::npos);
    EXPECT_NE(output.find("type=PEG_BID"), std::string::npos);
    EXPECT_NE(output.find("type=PEG_OFFER"), std::string::npos);
}

TEST(FormattingTest, FormatsOnePriceLevel) {
    const OrderBookSnapshot snapshot = sample_snapshot();

    EXPECT_EQ(
        format_price_level(snapshot, Side::Buy, 10'50),
        "BUY 10.5 total=50\n"
        "  id=3 qty=30 seq=2 type=LIMIT\n"
        "  id=2 qty=20 seq=4 type=PEG_BID\n"
    );

    EXPECT_EQ(
        format_price_level(snapshot, Side::Sell, 12'00),
        "Price level not found: SELL 12\n"
    );
}

TEST(FormattingTest, FormatsActiveAndTerminalOrders) {
    Order limit = Order::limit(8, Side::Buy, 10'50, 20, 6);
    Order market = Order::market(9, Side::Sell, 10, 7);

    ASSERT_TRUE(market.cancel());

    EXPECT_EQ(
        format_order(limit),
        "ORDER 8\n"
        "  side=BUY\n"
        "  type=LIMIT\n"
        "  price=10.5\n"
        "  original_qty=20\n"
        "  remaining_qty=20\n"
        "  sequence=6\n"
        "  status=ACTIVE\n"
    );

    EXPECT_NE(format_order(market).find("type=MARKET"), std::string::npos);
    EXPECT_NE(format_order(market).find("price=MARKET"), std::string::npos);
    EXPECT_NE(format_order(market).find("status=CANCELLED"), std::string::npos);
}

}  // namespace
}  // namespace matching_engine
