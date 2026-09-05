#pragma once

#include "matching_engine/order.hpp"
#include "matching_engine/order_book.hpp"

#include <string>

namespace matching_engine {

std::string format_price(Price price);

std::string format_book_summary(
    const OrderBookSnapshot& snapshot
);

std::string format_book_detailed(
    const OrderBookSnapshot& snapshot
);

std::string format_price_level(
    const OrderBookSnapshot& snapshot,
    Side side,
    Price price
);

std::string format_order(const Order& order);

}  // namespace matching_engine
