#include "matching_engine/console.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace matching_engine {
namespace {

TEST(ConsoleTest, PrintsIndividualOrdersAndSummaryBookSideBySide) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit buy 10.00 10"));
    ASSERT_TRUE(console.execute("limit buy 10.50 20"));
    ASSERT_TRUE(console.execute("limit sell 11.00 30"));
    ASSERT_TRUE(console.execute("limit sell 10.75 40"));

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("print book"));
    const std::string book = output.str();

    EXPECT_NE(book.find("Ordens de Compra"), std::string::npos);
    EXPECT_NE(book.find("Ordens de Venda"), std::string::npos);
    EXPECT_NE(book.find("20 @ 10.5"), std::string::npos);
    EXPECT_NE(book.find("40 @ 10.75"), std::string::npos);
    EXPECT_LT(book.find("20 @ 10.5"), book.find("10 @ 10"));
    EXPECT_LT(book.find("40 @ 10.75"), book.find("30 @ 11"));
    EXPECT_EQ(book.find("id="), std::string::npos);
    EXPECT_EQ(book.find("total="), std::string::npos);

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("print book summary"));
    const std::string summary = output.str();

    EXPECT_NE(summary.find("PRICE"), std::string::npos);
    EXPECT_NE(summary.find("ORDERS"), std::string::npos);
    EXPECT_EQ(summary.find("id="), std::string::npos);
}

TEST(ConsoleTest, PrintsOneLevelAndOneOrder) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit buy 10.50 20"));
    ASSERT_TRUE(console.execute("peg bid buy 15"));

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("print level buy 10.50"));
    EXPECT_NE(output.str().find("BUY 10.5 total=35"), std::string::npos);
    EXPECT_NE(output.str().find("type=PEG_BID"), std::string::npos);

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("print order 2"));
    EXPECT_NE(output.str().find("ORDER 2"), std::string::npos);
    EXPECT_NE(output.str().find("type=PEG_BID"), std::string::npos);
    EXPECT_NE(output.str().find("status=ACTIVE"), std::string::npos);
}

TEST(ConsoleTest, ReportsTradesCancellationsAndAmendments) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit sell 10.00 10"));
    ASSERT_TRUE(console.execute("market buy 15"));
    ASSERT_TRUE(console.execute("limit buy 9.50 20"));
    ASSERT_TRUE(console.execute("amend price 3 9.75"));
    ASSERT_TRUE(console.execute("amend quantity 3 10"));
    ASSERT_TRUE(console.execute("cancel order 3"));

    const std::string text = output.str();

    EXPECT_NE(text.find("Trade, price: 10, qty: 10"), std::string::npos);
    EXPECT_NE(
        text.find(
            "Order cancelled: id=2, qty=5, reason=MARKET_REMAINDER"
        ),
        std::string::npos
    );
    EXPECT_NE(text.find("Order amended: id=3"), std::string::npos);
    EXPECT_NE(
        text.find(
            "Order cancelled: id=3, qty=10, reason=USER_REQUESTED"
        ),
        std::string::npos
    );
}

TEST(ConsoleTest, AggregatesAssignmentTradesAtTheSamePrice) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit buy 10 100"));
    ASSERT_TRUE(console.execute("limit sell 20 100"));
    ASSERT_TRUE(console.execute("limit sell 20 200"));

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("market buy 150"));

    const std::string first_market = output.str();
    const std::string trade = "Trade, price: 20, qty: 150";
    const std::size_t first_trade = first_market.find(trade);

    ASSERT_NE(first_trade, std::string::npos);
    EXPECT_EQ(
        first_market.find("Trade, price:", first_trade + trade.size()),
        std::string::npos
    );

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("market buy 200"));
    EXPECT_NE(
        output.str().find("Trade, price: 20, qty: 150"),
        std::string::npos
    );

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("market sell 200"));
    EXPECT_NE(
        output.str().find("Trade, price: 10, qty: 100"),
        std::string::npos
    );
}

TEST(ConsoleTest, PrintsAssignmentPegBeforeNewLimitAfterRepricing) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit buy 10 200"));
    ASSERT_TRUE(console.execute("limit buy 9.99 100"));
    ASSERT_TRUE(console.execute("limit sell 10.5 100"));
    ASSERT_TRUE(console.execute("peg bid buy 150"));
    ASSERT_TRUE(console.execute("limit buy 10.1 300"));

    output.str("");
    output.clear();

    ASSERT_TRUE(console.execute("print book"));

    const std::string book = output.str();
    const std::size_t peg = book.find("150 @ 10.1");
    const std::size_t new_limit = book.find("300 @ 10.1");

    ASSERT_NE(peg, std::string::npos);
    ASSERT_NE(new_limit, std::string::npos);
    EXPECT_LT(peg, new_limit);
}

TEST(ConsoleTest, AppliesCombinedAmendmentAtomically) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit sell 10.00 150"));
    ASSERT_TRUE(console.execute("limit buy 9.00 100"));
    ASSERT_TRUE(console.execute(
        "AMEND ORDER 2 Quantity 200 Price 10.00"
    ));
    ASSERT_TRUE(console.execute("print order 2"));

    const std::string text = output.str();

    EXPECT_NE(text.find("Order amended: id=2"), std::string::npos);
    EXPECT_NE(text.find("Trade, price: 10, qty: 150"), std::string::npos);
    EXPECT_NE(text.find("price=10"), std::string::npos);
    EXPECT_NE(text.find("remaining_qty=50"), std::string::npos);
    EXPECT_NE(text.find("sequence=3"), std::string::npos);
}

TEST(ConsoleTest, RejectsMalformedInputAndReportsEngineErrors) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    ASSERT_TRUE(console.execute("limit hold 10.00 5"));
    ASSERT_TRUE(console.execute("limit buy 10.001 5"));
    ASSERT_TRUE(console.execute("market buy nope"));
    ASSERT_TRUE(console.execute("cancel order 999"));
    ASSERT_TRUE(console.execute("unknown"));

    const std::string text = output.str();

    EXPECT_NE(text.find("Error: invalid side"), std::string::npos);
    EXPECT_NE(text.find("Error: invalid price"), std::string::npos);
    EXPECT_NE(text.find("Error: invalid quantity"), std::string::npos);
    EXPECT_NE(text.find("Error: order not found"), std::string::npos);
    EXPECT_NE(text.find("Error: unknown command"), std::string::npos);
}

TEST(ConsoleTest, PrintsHelpAndStopsOnExit) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    EXPECT_TRUE(console.execute("help"));
    EXPECT_FALSE(console.execute("exit"));

    EXPECT_EQ(output.str().find("print book detailed"), std::string::npos);
    EXPECT_NE(output.str().find("print book summary"), std::string::npos);
    EXPECT_NE(output.str().find("Bye"), std::string::npos);
}

TEST(ConsoleTest, AcceptsCommandsWithDifferentLetterCases) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);

    EXPECT_TRUE(console.execute("LiMiT BUY 10.50 20"));
    EXPECT_TRUE(console.execute("PEG Bid Buy 10"));
    EXPECT_TRUE(console.execute("PRINT BOOK"));

    const std::string text = output.str();

    EXPECT_NE(text.find("Order created: buy 20 @ 10.5, id: 1"), std::string::npos);
    EXPECT_NE(text.find("Order created: peg bid buy 10 @ 10.5, id: 2"), std::string::npos);
    EXPECT_NE(text.find("20 @ 10.5"), std::string::npos);
    EXPECT_NE(text.find("10 @ 10.5"), std::string::npos);
}

TEST(ConsoleTest, BookKeepsSamePriceOrdersSeparateWhileSummaryAggregates) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);
    console.execute("limit sell 20 100");
    console.execute("limit sell 20 200");
    output.str("");
    console.execute("print book");
    const auto book = output.str();
    ASSERT_NE(book.find("100 @ 20"), std::string::npos);
    ASSERT_NE(book.find("200 @ 20"), std::string::npos);
    EXPECT_LT(book.find("100 @ 20"), book.find("200 @ 20"));
    EXPECT_EQ(book.find("300 @ 20"), std::string::npos);
    output.str("");
    console.execute("print book summary");
    EXPECT_NE(output.str().find("300"), std::string::npos);
    EXPECT_EQ(output.str().find('@'), std::string::npos);
}

TEST(ConsoleTest, BookReflectsRemainingQuantityCancellationAndPriceAmendments) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);
    console.execute("limit buy 10 200");
    console.execute("limit buy 9.99 100");
    console.execute("limit sell 10.5 100");
    console.execute("amend price 1 9.98");
    console.execute("market sell 40");
    output.str("");
    console.execute("print book");
    EXPECT_EQ(output.str(),
        "Ordens de Compra   | Ordens de Venda\n"
        "-------------------+-------------------\n"
        "60 @ 9.99          | 100 @ 10.5\n"
        "200 @ 9.98         | \n");
    console.execute("cancel order 1");
    output.str("");
    console.execute("print book");
    EXPECT_EQ(output.str().find("200 @ 9.98"), std::string::npos);
    EXPECT_NE(output.str().find("60 @ 9.99"), std::string::npos);
}

TEST(ConsoleTest, RejectsRemovedDetailedBookCommand) {
    std::istringstream input;
    std::ostringstream output;
    Console console(input, output);
    console.execute("limit buy 10 100");
    output.str("");
    console.execute("print book detailed");
    EXPECT_EQ(output.str(), "Error: invalid print command\n");
    output.str("");
    console.execute("print level buy 10");
    EXPECT_NE(output.str().find("id=1 qty=100 seq=1 type=LIMIT"), std::string::npos);
}

}  // namespace
}  // namespace matching_engine
