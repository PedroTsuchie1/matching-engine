#pragma once

#include "matching_engine/order.hpp"
#include "matching_engine/order_book.hpp"

#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace matching_engine {

enum class EngineError {
    InvalidPrice,
    InvalidQuantity,
    UnsupportedPegCombination,
    PegReferenceUnavailable,
    OrderNotFound,
    OrderNotOpen,
    UnsupportedAmendment
};

struct Trade {
    OrderId resting_order_id;
    OrderId aggressive_order_id;
    Price price;
    Quantity quantity;
};

struct SubmissionReport {
    OrderId order_id;
    std::vector<Trade> trades;
    std::vector<OrderId> cancelled_order_ids;
};

using SubmissionResult =
    std::variant<SubmissionReport, EngineError>;

struct CancellationReport {
    OrderId order_id;
    std::vector<OrderId> cancelled_order_ids;
};

using CancellationResult = std::variant<CancellationReport, EngineError>;

using AmendmentReport = SubmissionReport;

using AmendmentResult =
    std::variant<AmendmentReport, EngineError>;

class MatchingEngine {
public:
    SubmissionResult submit_limit(
        Side side,
        Price price,
        Quantity quantity
    );

    SubmissionResult submit_market(
        Side side, Quantity quantity
    );

    SubmissionResult submit_peg(
        Side side, PegReference peg_reference, Quantity quantity
    );

    CancellationResult cancel_order(OrderId order_id);

    AmendmentResult amend_quantity(
        OrderId order_id,
        Quantity new_remaining_quantity
    );

    AmendmentResult amend_price(
        OrderId order_id,
        Price new_price
    );

    const Order* find_order(OrderId id) const;
    const OrderBook& order_book() const;

    long long order_count() const;

private:
    std::vector<Trade> match(Order& aggressive_order);

    void refresh_all_pegs(std::vector<OrderId>& cancelled_order_ids);

    void refresh_pegs(
        PegReference peg_reference, std::vector<OrderId>& cancelled_order_ids
    );

    std::unordered_map<OrderId, Order> orders_by_id_;

    std::unordered_set<OrderId> bid_pegs_;
    std::unordered_set<OrderId> offer_pegs_;

    OrderBook order_book_;

    OrderId next_order_id_{1};
    Sequence next_sequence_{1};
};

}  // namespace matching_engine
