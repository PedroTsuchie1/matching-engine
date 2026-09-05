#include "matching_engine/order_book.hpp"

#include <algorithm>

namespace matching_engine {
namespace {

template <typename SideBook>
std::vector<BookLevelSnapshot> make_snapshot_levels(
    const SideBook& side_book
) {
    std::vector<BookLevelSnapshot> levels;
    levels.reserve(side_book.size());

    for (const auto& [price, price_level] : side_book) {
        BookLevelSnapshot level{
            price,
            0,
            {}
        };

        level.orders.reserve(price_level.size());

        for (const Order* order : price_level) {
            level.total_quantity += order->remaining_quantity();

            level.orders.push_back(
                BookOrderSnapshot{
                    order->id(),
                    order->remaining_quantity(),
                    order->sequence(),
                    order->peg_reference()
                }
            );
        }

        levels.push_back(level);
    }

    return levels;
}

}  // namespace

bool OrderBook::add(Order& order) {
    const std::optional<Price> price = order.price();

    if (order.status() != OrderStatus::Active || !price.has_value()) {
        return false;
    }

    if (order_index_.contains(order.id())) {
        return false;
    }

    PriceLevel* price_level = nullptr;

    if (order.side() == Side::Buy) {
        price_level = &buys_[price.value()];
    } else {
        price_level = &sells_[price.value()];
    }

    const auto insertion_point = std::find_if(
        price_level->begin(),
        price_level->end(),
        [&order](const Order* resting_order) {
            return resting_order->sequence() > order.sequence();
        }
    );

    const PriceLevel::iterator order_iterator =
        price_level->insert(insertion_point, &order);

    order_index_.emplace(order.id(), order_iterator);

    return true;
}

Order* OrderBook::find(OrderId id) {
    const auto index_iterator = order_index_.find(id);

    if (index_iterator == order_index_.end()) {
        return nullptr;
    }

    return *index_iterator->second;
}

const Order* OrderBook::find(OrderId id) const {
    const auto index_iterator = order_index_.find(id);

    if (index_iterator == order_index_.end()) {
        return nullptr;
    }

    return *index_iterator->second;
}

Order* OrderBook::best(Side side) {
    if (side == Side::Buy) {
        if (buys_.empty()) {
            return nullptr;
        }

        return buys_.begin()->second.front();
    }

    if (sells_.empty()) {
        return nullptr;
    }

    return sells_.begin()->second.front();
}

const Order* OrderBook::best(Side side) const {
    if (side == Side::Buy) {
        if (buys_.empty()) {
            return nullptr;
        }

        return buys_.begin()->second.front();
    }

    if (sells_.empty()) {
        return nullptr;
    }

    return sells_.begin()->second.front();
}

std::optional<Price> OrderBook::best_limit_price(Side side) const {
    if (side == Side::Buy) {
        for (const auto& [price, price_level] : buys_) {
            for (const Order* order : price_level) {
                if (order->type() == OrderType::Limit &&
                    !order->is_pegged()) {
                    return price;
                }
            }
        }

        return std::nullopt;
    }

    for (const auto& [price, price_level] : sells_) {
        for (const Order* order : price_level) {
            if (order->type() == OrderType::Limit &&
                !order->is_pegged()) {
                return price;
            }
        }
    }

    return std::nullopt;
}

OrderBookSnapshot OrderBook::snapshot() const {
    return OrderBookSnapshot{
        make_snapshot_levels(buys_),
        make_snapshot_levels(sells_)
    };
}

bool OrderBook::remove(OrderId id) {
    const auto index_iterator = order_index_.find(id);

    if (index_iterator == order_index_.end()) {
        return false;
    }

    const PriceLevel::iterator order_iterator = index_iterator->second;
    Order* order = *order_iterator;
    const Price price = order->price().value();

    if (order->side() == Side::Buy) {
        auto price_level_iterator = buys_.find(price);
        PriceLevel& price_level = price_level_iterator->second;

        price_level.erase(order_iterator);

        if (price_level.empty()) {
            buys_.erase(price_level_iterator);
        }
    } else {
        auto price_level_iterator = sells_.find(price);
        PriceLevel& price_level = price_level_iterator->second;

        price_level.erase(order_iterator);

        if (price_level.empty()) {
            sells_.erase(price_level_iterator);
        }
    }

    order_index_.erase(index_iterator);

    return true;
}

long long OrderBook::size() const {
    return static_cast<long long>(order_index_.size());
}

bool OrderBook::empty() const {
    return order_index_.empty();
}

}
