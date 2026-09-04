#pragma once
#include <cstdint>
#include <optional>

namespace matching_engine {

using OrderId = std::uint64_t;
using Sequence = std::uint64_t;
using Quantity = std::uint64_t;
using Price = std::int64_t;

enum class OrderType {
    Limit,
    Market,
    Pegged
};

enum class Side {
    Buy,
    Sell
};

enum class PegReference {
    None,
    Bid,
    Offer
};

enum class OrderStatus {
    Active,
    Inactive,
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
        std::optional<Price> price = std::nullopt
    );

    bool apply_peg_reference(std::optional<Price> reference_price);
    bool cancel();

    OrderId id() const;
    OrderType type() const;
    Side side() const;
    std::optional<Price> price() const;
    Quantity original_quantity() const;
    Quantity remaining_quantity() const;
    Sequence sequence() const;
    PegReference peg_reference() const;
    OrderStatus status() const;

private:
    Order(
        OrderId id,
        OrderType type,
        Side side,
        std::optional<Price> price,
        Quantity quantity,
        Sequence sequence,
        PegReference peg_reference,
        OrderStatus status
    );

    OrderId id_;
    OrderType type_;
    Side side_;
    std::optional<Price> price_;
    Quantity original_quantity_;
    Quantity remaining_quantity_;
    Sequence sequence_;
    PegReference peg_reference_;
    OrderStatus status_;
};

}  // namespace matching_engine
