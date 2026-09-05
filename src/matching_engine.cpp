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
    if (price <= 0)
        return EngineError::InvalidPrice;

    if (quantity <= 0)
        return EngineError::InvalidQuantity;

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

    std::vector<Cancellation> cancellations;

    if (aggressive_order.status() == OrderStatus::Active)
        order_book_.add(aggressive_order);
    
    refresh_all_pegs(cancellations);
    return SubmissionReport{
        order_id,
        trades,
        cancellations
    };
}

SubmissionResult MatchingEngine::submit_market(
    Side side, Quantity quantity
) {
    if (quantity <= 0)
        return EngineError::InvalidQuantity;

    const OrderId order_id = next_order_id_++;
    const Sequence sequence = next_sequence_++;

    auto order_iterator = orders_by_id_.emplace(
        order_id, Order::market(
            order_id, side, quantity, sequence)
    ).first;

    Order& aggressive_order = order_iterator->second;

    std::vector<Trade> trades = match(aggressive_order);
    std::vector<Cancellation> cancellations;

    if (aggressive_order.status() == OrderStatus::Active) {
        const Quantity cancelled_quantity =
            aggressive_order.remaining_quantity();

        aggressive_order.cancel();
        cancellations.push_back(
            Cancellation{
                order_id,
                cancelled_quantity,
                CancellationReason::MarketRemainder
            }
        );
    }
    refresh_all_pegs(cancellations);
    return SubmissionReport{
        order_id,
        trades,
        cancellations
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

CancellationResult MatchingEngine::cancel_order(OrderId order_id) {
    const auto order_iterator = orders_by_id_.find(order_id);

    if (order_iterator == orders_by_id_.end())
        return EngineError::OrderNotFound;

    Order& order = order_iterator->second;

    if (order.status() != OrderStatus::Active)
        return EngineError::OrderNotOpen;

    order_book_.remove(order_id);

    if (order.is_pegged()) {
        const PegReference peg_reference =
            order.peg_reference().value();

        if (peg_reference == PegReference::Bid)
            bid_pegs_.erase(order_id);
        else
            offer_pegs_.erase(order_id);
    }

    const Quantity cancelled_quantity = order.remaining_quantity();

    order.cancel();

    std::vector<Cancellation> cancellations{
        Cancellation{
            order_id,
            cancelled_quantity,
            CancellationReason::UserRequested
        }
    };

    refresh_all_pegs(cancellations);

    return CancellationReport{
        order_id,
        cancellations
    };
}

AmendmentResult MatchingEngine::amend_quantity(
    OrderId order_id,
    Quantity new_remaining_quantity
) {
    return amend_order(
        order_id,
        AmendmentRequest{
            std::nullopt,
            new_remaining_quantity
        }
    );
}

AmendmentResult MatchingEngine::amend_price(
    OrderId order_id,
    Price new_price
) {
    return amend_order(
        order_id,
        AmendmentRequest{
            new_price,
            std::nullopt
        }
    );
}

AmendmentResult MatchingEngine::amend_order(
    OrderId order_id,
    const AmendmentRequest& request
) {
    if (!request.price.has_value() &&
        !request.remaining_quantity.has_value()) {
        return EngineError::EmptyAmendment;
    }

    if (request.price.has_value() && request.price.value() <= 0)
        return EngineError::InvalidPrice;

    if (request.remaining_quantity.has_value() &&
        request.remaining_quantity.value() <= 0) {
        return EngineError::InvalidQuantity;
    }

    const auto order_iterator = orders_by_id_.find(order_id);

    if (order_iterator == orders_by_id_.end())
        return EngineError::OrderNotFound;

    Order& order = order_iterator->second;

    if (order.status() != OrderStatus::Active)
        return EngineError::OrderNotOpen;

    if (request.price.has_value() &&
        (order.type() != OrderType::Limit || order.is_pegged())) {
        return EngineError::UnsupportedAmendment;
    }

    const bool price_changes =
        request.price.has_value() &&
        request.price.value() != order.price().value();

    const Quantity new_remaining_quantity =
        request.remaining_quantity.value_or(
            order.remaining_quantity()
        );

    const bool quantity_changes =
        new_remaining_quantity != order.remaining_quantity();

    if (!price_changes && !quantity_changes) {
        return AmendmentReport{
            order_id,
            {},
            {}
        };
    }

    const bool loses_priority =
        price_changes ||
        new_remaining_quantity > order.remaining_quantity();

    if (!loses_priority) {
        order.apply_quantity_amendment(
            new_remaining_quantity,
            order.sequence()
        );

        return AmendmentReport{
            order_id,
            {},
            {}
        };
    }

    order_book_.remove(order_id);

    const Sequence new_sequence = next_sequence_++;

    if (request.remaining_quantity.has_value()) {
        order.apply_quantity_amendment(
            new_remaining_quantity,
            new_sequence
        );
    }

    if (price_changes) {
        order.apply_price_amendment(
            request.price.value(),
            new_sequence
        );
    }

    if (!price_changes) {
        order_book_.add(order);

        return AmendmentReport{
            order_id,
            {},
            {}
        };
    }

    std::vector<Trade> trades = match(order);

    if (order.status() == OrderStatus::Active)
        order_book_.add(order);

    std::vector<Cancellation> cancellations;
    refresh_all_pegs(cancellations);

    return AmendmentReport{
        order_id,
        trades,
        cancellations
    };
}

void MatchingEngine::refresh_all_pegs(
    std::vector<Cancellation>& cancellations
) {
    refresh_pegs(
        PegReference::Bid,
        cancellations
    );

    refresh_pegs(
        PegReference::Offer,
        cancellations
    );
}

void MatchingEngine::refresh_pegs(
    PegReference peg_reference,
    std::vector<Cancellation>& cancellations
) {
    std::unordered_set<OrderId>& peg_ids =
        peg_reference == PegReference::Bid
            ? bid_pegs_
            : offer_pegs_;

    const Side reference_side =
        peg_reference == PegReference::Bid
            ? Side::Buy
            : Side::Sell;

    const std::optional<Price> reference_price =
        order_book_.best_limit_price(reference_side);

    std::vector<OrderId> ordered_peg_ids(
        peg_ids.begin(),
        peg_ids.end()
    );

    std::sort(
        ordered_peg_ids.begin(),
        ordered_peg_ids.end(),
        [this](OrderId left_id, OrderId right_id) {
            return orders_by_id_.at(left_id).sequence() <
                   orders_by_id_.at(right_id).sequence();
        }
    );

    for (OrderId order_id : ordered_peg_ids) {
        Order& peg_order = orders_by_id_.at(order_id);

        if (peg_order.status() != OrderStatus::Active) {
            peg_ids.erase(order_id);
            continue;
        }

        if (!reference_price.has_value()) {
            order_book_.remove(order_id);

            const Quantity cancelled_quantity =
                peg_order.remaining_quantity();

            peg_order.cancel();

            cancellations.push_back(
                Cancellation{
                    order_id,
                    cancelled_quantity,
                    CancellationReason::PegReferenceUnavailable
                }
            );
            peg_ids.erase(order_id);

            continue;
        }

        if (peg_order.price().value() == reference_price.value())
            continue;

        order_book_.remove(order_id);

        peg_order.apply_peg_reference(reference_price.value());

        order_book_.add(peg_order);
    }
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
