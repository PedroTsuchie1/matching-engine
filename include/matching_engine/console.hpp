#pragma once

#include "matching_engine/matching_engine.hpp"

#include <iosfwd>
#include <optional>
#include <string>

namespace matching_engine {

class Console {
public:
    Console(std::istream& input, std::ostream& output);

    void run();
    bool execute(const std::string& line);

private:
    struct PendingLimit {
        Side side;
        Price price;
        Quantity quantity;
    };

    enum class ResultAction {
        Created,
        Amended
    };

    void print_help();
    void submit_limit(Side side, Price price, Quantity quantity);
    bool confirm_limit(const std::string& line);
    void print_error(EngineError error);

    void print_order_result(
        const SubmissionResult& result,
        ResultAction action
    );

    void print_cancellation_result(
        const CancellationResult& result
    );

    MatchingEngine engine_;
    std::optional<PendingLimit> pending_limit_;
    std::istream& input_;
    std::ostream& output_;
};

}  // namespace matching_engine
