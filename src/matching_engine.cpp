#include "matching_engine/matching_engine.hpp"
#include <algorithm>

namespace matching_engine {
namespace {

Side opposite_side(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

bool can_match(
    const Order& aggressive_order,
    const Order& resting_order
) {
    if (aggressive_order.type() == OrderType::Market) {
        return true;
    }

    const Price aggressive_price = aggressive_order.price().value();
    const Price resting_price = resting_order.price().value();

    if (aggressive_order.side() == Side::Buy) {
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
    std::vector<Trade> trades = match(aggressive_order);

    if (aggressive_order.status() == OrderStatus::Active)
        order_book_.add(aggressive_order);
    
    return SubmissionReport{
        order_id,
        trades,
        {}
    };
}

SubmissionResult MatchingEngine::submit_market(
    Side side, Quantity quantity
) {
    if (quantity <= 0) {
        return EngineError::InvalidQuantity;
    }

    const OrderId order_id = next_order_id_++;
    const Sequence sequence = next_sequence_++;

    auto order_iterator = orders_by_id_.emplace(
        order_id, Order::market(
            order_id, side, quantity, sequence)
    ).first;

    Order& aggressive_order = order_iterator->second;

    std::vector<Trade> trades = match(aggressive_order);
    std::vector<OrderId> cancelled_order_ids;

    if (aggressive_order.status() == OrderStatus::Active) {
        aggressive_order.cancel();
        cancelled_order_ids.push_back(order_id);
    }

    return SubmissionReport{
        order_id,
        trades,
        cancelled_order_ids
    };
}

SubmissionResult MatchingEngine::submit_peg(
    Side side, PegReference peg_reference, Quantity quantity
) {
    if (quantity <= 0)
        return EngineError::InvalidQuantity;

    const bool supported_combination =
        (side == Side::Buy &&
         peg_reference == PegReference::Bid) ||
        (side == Side::Sell &&
         peg_reference == PegReference::Offer);

    if (!supported_combination)
        return EngineError::UnsupportedPegCombination;

    const Side reference_side =
        peg_reference == PegReference::Bid
            ? Side::Buy
            : Side::Sell;

    const std::optional<Price> reference_price =
        order_book_.best_limit_price(reference_side);

    if (!reference_price.has_value())
        return EngineError::PegReferenceUnavailable;

    const OrderId order_id = next_order_id_++;
    const Sequence sequence = next_sequence_++;

    auto order_iterator = orders_by_id_.emplace(
        order_id,
        Order::pegged(
            order_id,
            side,
            peg_reference,
            quantity,
            sequence,
            reference_price.value()
        )
    ).first;

    Order& peg_order = order_iterator->second;

    order_book_.add(peg_order);

    if (peg_reference == PegReference::Bid)
        bid_pegs_.insert(order_id);
    else
        offer_pegs_.insert(order_id);

    return SubmissionReport{
        order_id,
        {},
        {}
    };
}

std::vector<Trade> MatchingEngine::match(Order& aggressive_order) {
    std::vector<Trade> trades;

    const Side resting_side = opposite_side(aggressive_order.side());

    while (aggressive_order.remaining_quantity() > 0) {
        Order* resting_order = order_book_.best(resting_side);

        if (resting_order == nullptr) 
            break;
        if (!can_match(aggressive_order, *resting_order))
            break;
        
        const Quantity aggressive_quantity = aggressive_order.remaining_quantity();
        const Quantity resting_quantity = (*resting_order).remaining_quantity();
        const Quantity traded_quantity = std::min(resting_quantity, aggressive_quantity);

        const OrderId resting_order_id = resting_order->id();

        const Price trade_price = resting_order->price().value();

        aggressive_order.apply_fill(traded_quantity);
        resting_order->apply_fill(traded_quantity);
        Trade aux= {resting_order_id, aggressive_order.id(), trade_price, traded_quantity};
        trades.push_back(aux);

        if (resting_order->status() == OrderStatus::Filled) {
            order_book_.remove(resting_order_id);
        }
    }
    return trades;
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
