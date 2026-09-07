#include "matching_engine/formatting.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace matching_engine {
namespace {

constexpr int book_column_width = 45;

const char* side_label(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

const char* status_label(OrderStatus status) {
    if (status == OrderStatus::Active)
        return "ACTIVE";

    if (status == OrderStatus::Filled)
        return "FILLED";

    return "CANCELLED";
}

const char* snapshot_order_label(
    const BookOrderSnapshot& order
) {
    if (!order.peg_reference.has_value())
        return "LIMIT";

    if (order.peg_reference.value() == PegReference::Bid)
        return "PEG_BID";

    return "PEG_OFFER";
}

const char* order_label(const Order& order) {
    if (order.type() == OrderType::Market)
        return "MARKET";

    if (!order.is_pegged())
        return "LIMIT";

    if (order.peg_reference().value() == PegReference::Bid)
        return "PEG_BID";

    return "PEG_OFFER";
}

std::string summary_heading() {
    std::ostringstream output;

    output
        << std::left
        << std::setw(12)
        << "PRICE"
        << std::setw(12)
        << "QTY"
        << "ORDERS";

    return output.str();
}

std::string summary_level(
    const BookLevelSnapshot* level
) {
    if (level == nullptr)
        return "";

    std::ostringstream output;

    output
        << std::left
        << std::setw(12)
        << format_price(level->price)
        << std::setw(12)
        << level->total_quantity
        << level->orders.size();

    return output.str();
}

std::vector<std::string> book_order_lines(
    const std::vector<BookLevelSnapshot>& levels
) {
    std::vector<std::string> lines;
    for (const BookLevelSnapshot& level : levels) {
        for (const BookOrderSnapshot& order : level.orders) {
            lines.push_back(
                std::to_string(order.remaining_quantity) + " @ " +
                format_price(level.price)
            );
        }
    }
    return lines;
}

const BookLevelSnapshot* find_level(
    const OrderBookSnapshot& snapshot,
    Side side,
    Price price
) {
    const std::vector<BookLevelSnapshot>& levels =
        side == Side::Buy
            ? snapshot.buys
            : snapshot.sells;

    const auto level_iterator = std::find_if(
        levels.begin(),
        levels.end(),
        [price](const BookLevelSnapshot& level) {
            return level.price == price;
        }
    );

    if (level_iterator == levels.end())
        return nullptr;

    return &*level_iterator;
}

}  // namespace

std::string format_price(Price price) {
    std::ostringstream output;

    const Price whole = price / 100;
    const Price fraction = price % 100;

    output << whole;

    if (fraction == 0)
        return output.str();

    output << '.';

    if (fraction % 10 == 0) {
        output << fraction / 10;
    } else {
        output
            << std::setfill('0')
            << std::setw(2)
            << fraction;
    }

    return output.str();
}

std::string format_book_summary(
    const OrderBookSnapshot& snapshot
) {
    std::ostringstream output;

    output
        << std::left
        << std::setw(book_column_width)
        << "BUY"
        << " | SELL\n";

    output
        << std::setw(book_column_width)
        << summary_heading()
        << " | "
        << summary_heading()
        << '\n';

    const std::size_t depth = std::max(
        snapshot.buys.size(),
        snapshot.sells.size()
    );

    if (depth == 0) {
        output
            << std::setw(book_column_width)
            << "<empty>"
            << " | <empty>\n";

        return output.str();
    }

    for (std::size_t index = 0; index < depth; ++index) {
        const BookLevelSnapshot* buy_level =
            index < snapshot.buys.size()
                ? &snapshot.buys[index]
                : nullptr;

        const BookLevelSnapshot* sell_level =
            index < snapshot.sells.size()
                ? &snapshot.sells[index]
                : nullptr;

        output
            << std::setw(book_column_width)
            << summary_level(buy_level)
            << " | "
            << summary_level(sell_level)
            << '\n';
    }

    return output.str();
}

std::string format_book(
    const OrderBookSnapshot& snapshot
) {
    // Snapshot order already encodes price-time priority. Flatten each side
    // independently so two orders at one price remain two consecutive rows.
    const auto buys = book_order_lines(snapshot.buys);
    const auto sells = book_order_lines(snapshot.sells);
    int column_width = 18;
    for (const auto& line : buys)
        column_width = std::max(column_width, static_cast<int>(line.size()));
    for (const auto& line : sells)
        column_width = std::max(column_width, static_cast<int>(line.size()));

    std::ostringstream output;
    output << std::left << std::setw(column_width)
           << "Ordens de Compra" << " | Ordens de Venda\n"
           << std::string(column_width, '-') << "-+-"
           << std::string(column_width, '-') << '\n';

    const std::size_t rows = std::max(buys.size(), sells.size());
    if (rows == 0) {
        output << std::setw(column_width) << "<empty>" << " | <empty>\n";
    }
    for (std::size_t row = 0; row < rows; ++row) {
        output << std::setw(column_width) << (row < buys.size() ? buys[row] : "")
               << " | " << (row < sells.size() ? sells[row] : "") << '\n';
    }
    return output.str();
}

std::string format_price_level(
    const OrderBookSnapshot& snapshot,
    Side side,
    Price price
) {
    const BookLevelSnapshot* level =
        find_level(snapshot, side, price);

    if (level == nullptr) {
        std::ostringstream error;

        error
            << "Price level not found: "
            << side_label(side)
            << ' '
            << format_price(price)
            << '\n';

        return error.str();
    }

    std::ostringstream output;

    output
        << side_label(side)
        << ' '
        << format_price(level->price)
        << " total="
        << level->total_quantity
        << '\n';

    for (const BookOrderSnapshot& order : level->orders) {
        output
            << "  id="
            << order.order_id
            << " qty="
            << order.remaining_quantity
            << " seq="
            << order.sequence
            << " type="
            << snapshot_order_label(order)
            << '\n';
    }

    return output.str();
}

std::string format_order(const Order& order) {
    std::ostringstream output;

    output
        << "ORDER "
        << order.id()
        << '\n'
        << "  side="
        << side_label(order.side())
        << '\n'
        << "  type="
        << order_label(order)
        << '\n'
        << "  price=";

    if (order.price().has_value())
        output << format_price(order.price().value());
    else
        output << "MARKET";

    output
        << '\n'
        << "  original_qty="
        << order.original_quantity()
        << '\n'
        << "  remaining_qty="
        << order.remaining_quantity()
        << '\n'
        << "  sequence="
        << order.sequence()
        << '\n'
        << "  status="
        << status_label(order.status())
        << '\n';

    return output.str();
}

}  // namespace matching_engine
