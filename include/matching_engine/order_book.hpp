#pragma once

#include "matching_engine/order.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

namespace matching_engine {

class OrderBook {
public:
    bool add(Order& order);

    Order* find(OrderId id);
    const Order* find(OrderId id) const;
    Order* best(Side side);
    const Order* best(Side side) const;

    std::optional<Price> best_limit_price(Side side) const;

    bool remove(OrderId id);

    long long size() const;
    bool empty() const;

private:
    using PriceLevel = std::list<Order*>;

    using BuyBook = std::map<
        Price,
        PriceLevel,
        std::greater<Price>
    >;

    using SellBook = std::map<
        Price,
        PriceLevel,
        std::less<Price>
    >;

    BuyBook buys_;
    SellBook sells_;

    std::unordered_map<
        OrderId,
        PriceLevel::iterator
    > order_index_;
};

}  // namespace matching_engine
