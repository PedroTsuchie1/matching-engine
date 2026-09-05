#include "matching_engine/order.hpp"

namespace matching_engine {

Order Order::limit(
    OrderId id,
    Side side,
    Price price,
    Quantity quantity,
    Sequence sequence
) {
    return Order(
        id,
        OrderType::Limit,
        side,
        price,
        quantity,
        sequence,
        std::nullopt,
        OrderStatus::Active
    );
}

Order Order::market(
    OrderId id,
    Side side,
    Quantity quantity,
    Sequence sequence
) {
    return Order(
        id,
        OrderType::Market,
        side,
        std::nullopt,
        quantity,
        sequence,
        std::nullopt,
        OrderStatus::Active
    );
}

Order Order::pegged(
    OrderId id,
    Side side,
    PegReference peg_reference,
    Quantity quantity,
    Sequence sequence,
    Price price
) {
    return Order(
        id,
        OrderType::Limit,
        side,
        price,
        quantity,
        sequence,
        peg_reference,
        OrderStatus::Active
    );
}

bool Order::apply_peg_reference(Price reference_price, Sequence new_sequence) {
    if (!is_pegged()) {
        return false;
    }

    if (status_ == OrderStatus::Filled ||
        status_ == OrderStatus::Cancelled) {
        return false;
    }

    price_ = reference_price;
    sequence_ = new_sequence;

    return true;
}

bool Order::cancel() {
    if (status_ == OrderStatus::Filled ||
        status_ == OrderStatus::Cancelled) {
        return false;
    }

    remaining_quantity_ = 0;
    status_ = OrderStatus::Cancelled;

    return true;
}

bool Order::apply_fill(Quantity quantity) {
    if (status_ != OrderStatus::Active) {
        return false;
    }

    if (quantity <= 0 || quantity > remaining_quantity_) {
        return false;
    }

    remaining_quantity_ -= quantity;

    if (remaining_quantity_ == 0) {
        status_ = OrderStatus::Filled;
    }

    return true;
}

Order::Order(
    OrderId id,
    OrderType type,
    Side side,
    std::optional<Price> price,
    Quantity quantity,
    Sequence sequence,
    std::optional<PegReference> peg_reference,
    OrderStatus status
)
    : id_(id),
      type_(type),
      side_(side),
      price_(price),
      original_quantity_(quantity),
      remaining_quantity_(quantity),
      sequence_(sequence),
      peg_reference_(peg_reference),
      status_(status) {
}

OrderId Order::id() const {
    return id_;
}

OrderType Order::type() const {
    return type_;
}

Side Order::side() const {
    return side_;
}

std::optional<Price> Order::price() const {
    return price_;
}

Quantity Order::original_quantity() const {
    return original_quantity_;
}

Quantity Order::remaining_quantity() const {
    return remaining_quantity_;
}

Sequence Order::sequence() const {
    return sequence_;
}

std::optional<PegReference> Order::peg_reference() const {
    return peg_reference_;
}

bool Order::is_pegged() const {
    return peg_reference_.has_value();
}

OrderStatus Order::status() const {
    return status_;
}

}  // namespace matching_engine
