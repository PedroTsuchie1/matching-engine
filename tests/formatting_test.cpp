#include "matching_engine/formatting.hpp"

#include <gtest/gtest.h>

#include <string>
#include <sstream>
#include <limits>

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
    const std::string book = format_book(snapshot);

    EXPECT_LT(summary.find("BUY"), summary.find("SELL"));
    EXPECT_NE(summary.find("<empty>"), std::string::npos);
    EXPECT_LT(book.find("BUY"), book.find("SELL"));
    EXPECT_NE(book.find("<empty>"), std::string::npos);
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

TEST(FormattingTest, PrintsOneOrderPerRowWithoutGroupingPriceLevels) {
    EXPECT_EQ(
        format_book(sample_snapshot()),
        "BUY                            | SELL\n"
        "-------------------------------+-------------------------------\n"
        "30 @ 10.5                      | 40 @ 10.75\n"
        "20 @ 10.5                      | 30 @ 11\n"
        "10 @ 10                        | \n"
    );
}

TEST(FormattingTest, BookAndSummaryShareEnglishHeadingsAndSeparators) {
    for (const auto& snapshot : {OrderBookSnapshot{}, sample_snapshot()}) {
        std::istringstream book(format_book(snapshot));
        std::istringstream summary(format_book_summary(snapshot));
        std::string book_heading, summary_heading, columns, book_rule, summary_rule;
        std::getline(book, book_heading);
        std::getline(summary, summary_heading);
        EXPECT_EQ(book_heading, summary_heading);
        EXPECT_EQ(book_heading, "BUY                            | SELL");
        std::getline(book, book_rule);
        std::getline(summary, columns);
        std::getline(summary, summary_rule);
        EXPECT_EQ(book_rule, summary_rule);
        EXPECT_NE(columns.find("PRICE"), std::string::npos);
        EXPECT_NE(columns.find("QTY"), std::string::npos);
        EXPECT_NE(columns.find("ORDERS"), std::string::npos);
    }
}

TEST(FormattingTest, KeepsIdenticalOrdersAsSeparateLines) {
    auto snapshot = sample_snapshot();
    snapshot.buys[0].orders[1].remaining_quantity = 30;
    snapshot.buys[0].total_quantity = 60;
    const std::string output = format_book(snapshot);
    const auto first = output.find("30 @ 10.5");
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(output.find("30 @ 10.5", first + 1), std::string::npos);
    EXPECT_EQ(output.find("60 @ 10.5"), std::string::npos);
    for (const auto* field : {"id=", "seq=", "type=", "total="})
        EXPECT_EQ(output.find(field), std::string::npos);
}

TEST(FormattingTest, PrintsEitherSideAloneWithoutLosingOrders) {
    auto snapshot = sample_snapshot();
    snapshot.buys.clear();
    const auto sells = format_book(snapshot);
    EXPECT_NE(sells.find("                               | 40 @ 10.75\n"), std::string::npos);
    EXPECT_NE(sells.find("                               | 30 @ 11\n"), std::string::npos);
    snapshot = sample_snapshot();
    snapshot.sells.clear();
    EXPECT_NE(format_book(snapshot).find("10 @ 10                        | \n"), std::string::npos);
}

TEST(FormattingTest, ExpandsColumnsToKeepLargeValuesAligned) {
    auto snapshot = sample_snapshot();
    snapshot.buys[0].price = std::numeric_limits<Price>::max();
    snapshot.buys[0].orders[0].remaining_quantity = std::numeric_limits<Quantity>::max();
    const std::string output = format_book(snapshot);
    std::istringstream lines(output);
    std::string line;
    std::getline(lines, line);
    const auto separator = line.find('|');
    std::getline(lines, line); // Heading separator.
    while (std::getline(lines, line))
        EXPECT_EQ(line.find('|'), separator);
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
