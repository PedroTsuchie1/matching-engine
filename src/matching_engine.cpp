#include "matching_engine/matching_engine.hpp"

namespace matching_engine {
namespace {

Side opposite_side(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

bool crosses(
    Side aggressive_side,
    Price aggressive_price,
    Price resting_price
) {
    if (aggressive_side == Side::Buy) {
        return aggressive_price >= resting_price;
    }

    return aggressive_price <= resting_price;
}

}  // namespace

SubmissionResult MatchingEngine::submit_limit(
    Side side,
    Price price,
    Quantity quantity
) {
    if (price <= 0) {
        return EngineError::InvalidPrice;
    }

    if (quantity <= 0) {
        return EngineError::InvalidQuantity;
    }

    const OrderId order_id = next_order_id_++;
    const Sequence sequence = next_sequence_++;

    auto order_iterator = orders_by_id_.emplace(
        order_id,
        Order::limit(
            order_id,
            side,
            price,
            quantity,
            sequence
        )
    ).first;

    Order& aggressive_order = order_iterator->second;
    std::vector<Trade> trades;

    const Side resting_side = opposite_side(side);

    while (aggressive_order.remaining_quantity() > 0) {
        Order* resting_order = order_book_.best(resting_side);

        if (resting_order == nullptr) {
            break;
        }

        const Price resting_price =
            resting_order->price().value();

        if (!crosses(side, price, resting_price)) {
            break;
        }

        const Quantity aggressive_quantity =
            aggressive_order.remaining_quantity();
        const Quantity resting_quantity =
            resting_order->remaining_quantity();
        const Quantity traded_quantity =
            aggressive_quantity < resting_quantity
                ? aggressive_quantity
                : resting_quantity;

        const OrderId resting_order_id =
            resting_order->id();

        aggressive_order.apply_fill(traded_quantity);
        resting_order->apply_fill(traded_quantity);

        trades.push_back(Trade{
            resting_order_id,
            order_id,
            resting_price,
            traded_quantity
        });

        if (resting_order->status() == OrderStatus::Filled) {
            order_book_.remove(resting_order_id);
        }
    }

    if (aggressive_order.status() == OrderStatus::Active) {
        order_book_.add(aggressive_order);
    }

    return SubmissionReport{
        order_id,
        trades,
        {}
    };
}

const Order* MatchingEngine::find_order(OrderId id) const {
    const auto order_iterator = orders_by_id_.find(id);

    if (order_iterator == orders_by_id_.end()) {
        return nullptr;
    }

    return &order_iterator->second;
}

const OrderBook& MatchingEngine::order_book() const {
    return order_book_;
}

long long MatchingEngine::order_count() const {
    return static_cast<long long>(orders_by_id_.size());
}

}  // namespace matching_engine
