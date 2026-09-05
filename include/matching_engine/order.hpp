#pragma once

#include <optional>

namespace matching_engine {

using OrderId = long long;
using Sequence = long long;
using Quantity = long long;
using Price = long long;

enum class OrderType {
    Limit,
    Market
};

enum class Side {
    Buy,
    Sell
};

enum class PegReference {
    Bid,
    Offer
};

enum class OrderStatus {
    Active,
    Filled,
    Cancelled
};

class Order {
public:
    static Order limit(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Sequence sequence
    );

    static Order market(
        OrderId id,
        Side side,
        Quantity quantity,
        Sequence sequence
    );

    static Order pegged(
        OrderId id,
        Side side,
        PegReference peg_reference,
        Quantity quantity,
        Sequence sequence,
        Price price
    );

    bool apply_peg_reference(Price reference_price, Sequence new_sequence);
    bool cancel();
    bool apply_fill(Quantity quantity);

    OrderId id() const;
    OrderType type() const;
    Side side() const;
    std::optional<Price> price() const;
    Quantity original_quantity() const;
    Quantity remaining_quantity() const;
    Sequence sequence() const;
    std::optional<PegReference> peg_reference() const;
    bool is_pegged() const;
    OrderStatus status() const;

private:
    Order(
        OrderId id,
        OrderType type,
        Side side,
        std::optional<Price> price,
        Quantity quantity,
        Sequence sequence,
        std::optional<PegReference> peg_reference,
        OrderStatus status
    );

    OrderId id_;
    OrderType type_;
    Side side_;
    std::optional<Price> price_;
    Quantity original_quantity_;
    Quantity remaining_quantity_;
    Sequence sequence_;
    std::optional<PegReference> peg_reference_;
    OrderStatus status_;
};

}  // namespace matching_engine
