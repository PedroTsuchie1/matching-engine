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
        PegReference::None,
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
        PegReference::None,
        OrderStatus::Active
    );
}

Order Order::pegged(
    OrderId id,
    Side side,
    PegReference peg_reference,
    Quantity quantity,
    Sequence sequence,
    std::optional<Price> price
) {
    OrderStatus status = OrderStatus::Inactive;

    if (price.has_value()) {
        status = OrderStatus::Active;
    }

    return Order(
        id,
        OrderType::Pegged,
        side,
        price,
        quantity,
        sequence,
        peg_reference,
        status
    );
}

Order::Order(
    OrderId id,
    OrderType type,
    Side side,
    std::optional<Price> price,
    Quantity quantity,
    Sequence sequence,
    PegReference peg_reference,
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

PegReference Order::peg_reference() const {
    return peg_reference_;
}

OrderStatus Order::status() const {
    return status_;
}

}  // namespace matching_engine
