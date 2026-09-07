#include "matching_engine/matching_engine.hpp"

#include <charconv>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace matching_engine;

constexpr long long maximum_value = 1'000'000'000'000;
constexpr long long maximum_orders = 10'000;

struct Result {
    std::string error;
    std::optional<OrderId> order_id;
    std::vector<Trade> trades;
    std::vector<Cancellation> cancellations;
};

const char* error_name(EngineError error) {
    switch (error) {
    case EngineError::InvalidPrice: return "invalid_price";
    case EngineError::InvalidQuantity: return "invalid_quantity";
    case EngineError::UnsupportedPegCombination: return "unsupported_peg";
    case EngineError::PegReferenceUnavailable: return "reference_unavailable";
    case EngineError::OrderNotFound: return "order_not_found";
    case EngineError::OrderNotOpen: return "order_not_open";
    case EngineError::UnsupportedAmendment: return "unsupported_amendment";
    case EngineError::EmptyAmendment: return "empty_amendment";
    }
    return "bridge_error";
}

const char* side_name(Side side) { return side == Side::Buy ? "buy" : "sell"; }

std::optional<long long> positive_integer(const std::string& token) {
    long long value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        value <= 0 || value > maximum_value) {
        return std::nullopt;
    }
    return value;
}

Result from_submission(const SubmissionResult& result) {
    if (const auto* error = std::get_if<EngineError>(&result))
        return {error_name(*error), {}, {}, {}};
    const auto& report = std::get<SubmissionReport>(result);
    return {"", report.order_id, report.trades, report.cancellations};
}

Result execute(MatchingEngine& engine, const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    for (std::string token; input >> token;) tokens.push_back(token);
    if (tokens.size() == 1 && tokens[0] == "state") return {};
    if (tokens.empty()) return {"invalid_command", {}, {}, {}};
    const auto& action = tokens[0];
    if ((action == "limit" || action == "market" || action == "peg") &&
        engine.order_count() >= maximum_orders)
        return {"session_limit", {}, {}, {}};

    if ((action == "limit" && tokens.size() == 4) ||
        (action == "market" && tokens.size() == 3)) {
        if (tokens[1] != "buy" && tokens[1] != "sell")
            return {"invalid_side", {}, {}, {}};
        const auto side = tokens[1] == "buy" ? Side::Buy : Side::Sell;
        const auto quantity = positive_integer(tokens.back());
        if (!quantity) return {"invalid_quantity", {}, {}, {}};
        if (action == "market") return from_submission(engine.submit_market(side, *quantity));
        const auto price = positive_integer(tokens[2]);
        if (!price) return {"invalid_price", {}, {}, {}};
        return from_submission(engine.submit_limit(side, *price, *quantity));
    }
    if (action == "peg" && tokens.size() == 4) {
        if (tokens[1] != "bid" && tokens[1] != "offer")
            return {"unsupported_peg", {}, {}, {}};
        if (tokens[2] != "buy" && tokens[2] != "sell")
            return {"invalid_side", {}, {}, {}};
        const auto quantity = positive_integer(tokens[3]);
        if (!quantity) return {"invalid_quantity", {}, {}, {}};
        return from_submission(engine.submit_peg(
            tokens[2] == "buy" ? Side::Buy : Side::Sell,
            tokens[1] == "bid" ? PegReference::Bid : PegReference::Offer,
            *quantity));
    }
    if (action == "cancel" && tokens.size() == 2) {
        const auto id = positive_integer(tokens[1]);
        if (!id) return {"invalid_id", {}, {}, {}};
        const auto result = engine.cancel_order(*id);
        if (const auto* error = std::get_if<EngineError>(&result))
            return {error_name(*error), {}, {}, {}};
        const auto& report = std::get<CancellationReport>(result);
        return {"", report.order_id, {}, report.cancellations};
    }
    if (action == "amend" && tokens.size() == 4) {
        const auto id = positive_integer(tokens[1]);
        if (!id) return {"invalid_id", {}, {}, {}};
        AmendmentRequest request;
        if (tokens[2] != "-") {
            request.price = positive_integer(tokens[2]);
            if (!request.price) return {"invalid_price", {}, {}, {}};
        }
        if (tokens[3] != "-") {
            request.remaining_quantity = positive_integer(tokens[3]);
            if (!request.remaining_quantity) return {"invalid_quantity", {}, {}, {}};
        }
        return from_submission(engine.amend_order(*id, request));
    }
    return {"invalid_command", {}, {}, {}};
}

void write_order(std::ostream& out, const Order& order) {
    const char* type = order.is_pegged() ? "peg" :
        order.type() == OrderType::Market ? "market" : "limit";
    const char* status = order.status() == OrderStatus::Active ? "active" :
        order.status() == OrderStatus::Filled ? "filled" : "cancelled";
    out << "{\"id\":\"" << order.id() << "\",\"side\":\"" << side_name(order.side())
        << "\",\"type\":\"" << type << "\",\"price\":";
    if (order.price()) out << '"' << *order.price() << '"'; else out << "null";
    out << ",\"original_quantity\":\"" << order.original_quantity()
        << "\",\"remaining_quantity\":\"" << order.remaining_quantity()
        << "\",\"sequence\":\"" << order.sequence() << "\",\"peg_reference\":";
    if (order.peg_reference())
        out << '"' << (*order.peg_reference() == PegReference::Bid ? "bid" : "offer") << '"';
    else out << "null";
    out << ",\"status\":\"" << status << "\"}";
}

void write_levels(std::ostream& out, const MatchingEngine& engine,
                  const std::vector<BookLevelSnapshot>& levels) {
    out << '[';
    bool first_level = true;
    for (const auto& level : levels) {
        if (!first_level) out << ',';
        first_level = false;
        out << "{\"price\":\"" << level.price << "\",\"total_quantity\":\""
            << level.total_quantity << "\",\"orders\":[";
        bool first_order = true;
        for (const auto& entry : level.orders) {
            if (!first_order) out << ',';
            first_order = false;
            write_order(out, *engine.find_order(entry.order_id));
        }
        out << "]}";
    }
    out << ']';
}

void write_response(const MatchingEngine& engine, const Result& result) {
    const auto snapshot = engine.order_book().snapshot();
    auto& out = std::cout;
    out << "{\"ok\":" << (result.error.empty() ? "true" : "false");
    // Error strings are fixed protocol tokens, never user-provided text.
    if (!result.error.empty()) out << ",\"error_code\":\"" << result.error << '"';
    if (result.order_id) out << ",\"order_id\":\"" << *result.order_id << '"';
    out << ",\"book\":{\"buys\":";
    write_levels(out, engine, snapshot.buys);
    out << ",\"sells\":";
    write_levels(out, engine, snapshot.sells);
    out << "},\"orders\":[";
    for (OrderId id = engine.order_count(); id > 0; --id) {
        if (id != engine.order_count()) out << ',';
        write_order(out, *engine.find_order(id));
    }
    out << "],\"trades\":[";
    bool first = true;
    for (const auto& trade : result.trades) {
        if (!first) out << ',';
        first = false;
        out << "{\"resting_order_id\":\"" << trade.resting_order_id
            << "\",\"aggressive_order_id\":\"" << trade.aggressive_order_id
            << "\",\"price\":\"" << trade.price << "\",\"quantity\":\""
            << trade.quantity << "\",\"side\":\""
            << side_name(engine.find_order(trade.aggressive_order_id)->side()) << "\"}";
    }
    out << "],\"cancellations\":[";
    first = true;
    for (const auto& cancellation : result.cancellations) {
        if (!first) out << ',';
        first = false;
        const char* reason = cancellation.reason == CancellationReason::UserRequested
            ? "user_requested" : cancellation.reason == CancellationReason::MarketRemainder
            ? "market_remainder" : "reference_unavailable";
        out << "{\"order_id\":\"" << cancellation.order_id << "\",\"quantity\":\""
            << cancellation.quantity << "\",\"reason\":\"" << reason << "\"}";
    }
    out << "]}\n" << std::flush;
}
}  // namespace

int main() {
    MatchingEngine engine;
    for (std::string line; std::getline(std::cin, line);) {
        const Result result = execute(engine, line);
        write_response(engine, result);
    }
}
