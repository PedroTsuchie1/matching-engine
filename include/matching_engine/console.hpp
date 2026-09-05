#pragma once

#include "matching_engine/matching_engine.hpp"

#include <iosfwd>
#include <string>

namespace matching_engine {

class Console {
public:
    Console(std::istream& input, std::ostream& output);

    void run();
    bool execute(const std::string& line);

private:
    enum class ResultAction {
        Created,
        Amended
    };

    void print_help();
    void print_error(EngineError error);

    void print_order_result(
        const SubmissionResult& result,
        ResultAction action
    );

    void print_cancellation_result(
        const CancellationResult& result
    );

    MatchingEngine engine_;
    std::istream& input_;
    std::ostream& output_;
};

}  // namespace matching_engine
