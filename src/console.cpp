#include "matching_engine/console.hpp"

#include "matching_engine/formatting.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace matching_engine {
namespace {

std::string lowercase(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::tolower(character)
            );
        }
    );

    return text;
}

std::vector<std::string> split(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    std::string token;

    while (input >> token)
        tokens.push_back(lowercase(token));

    return tokens;
}

std::optional<long long> parse_integer(std::string_view text) {
    if (text.empty())
        return std::nullopt;

    long long value = 0;

    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }

    return value;
}

bool contains_only_digits(std::string_view text) {
    if (text.empty())
        return false;

    for (char character : text) {
        if (character < '0' || character > '9')
            return false;
    }

    return true;
}

std::optional<Price> parse_price(std::string_view text) {
    if (text.empty())
        return std::nullopt;

    bool negative = false;

    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }

    const std::size_t decimal_point = text.find('.');

    const std::string_view whole_text =
        decimal_point == std::string_view::npos
            ? text
            : text.substr(0, decimal_point);

    if (!contains_only_digits(whole_text))
        return std::nullopt;

    std::string_view fraction_text;

    if (decimal_point != std::string_view::npos) {
        if (text.find('.', decimal_point + 1) != std::string_view::npos)
            return std::nullopt;

        fraction_text = text.substr(decimal_point + 1);

        if (!contains_only_digits(fraction_text) ||
            fraction_text.size() > 2) {
            return std::nullopt;
        }
    }

    const std::optional<long long> whole = parse_integer(whole_text);

    if (!whole.has_value())
        return std::nullopt;

    long long fraction = 0;

    if (!fraction_text.empty()) {
        fraction = parse_integer(fraction_text).value();

        if (fraction_text.size() == 1)
            fraction *= 10;
    }

    if (whole.value() >
        (std::numeric_limits<Price>::max() - fraction) / 100) {
        return std::nullopt;
    }

    const Price price = whole.value() * 100 + fraction;
    return negative ? -price : price;
}

std::optional<Side> parse_side(const std::string& token) {
    if (token == "buy")
        return Side::Buy;

    if (token == "sell")
        return Side::Sell;

    return std::nullopt;
}

std::optional<PegReference> parse_peg_reference(
    const std::string& token
) {
    if (token == "bid")
        return PegReference::Bid;

    if (token == "offer")
        return PegReference::Offer;

    return std::nullopt;
}

const char* error_message(EngineError error) {
    if (error == EngineError::InvalidPrice)
        return "invalid price";

    if (error == EngineError::InvalidQuantity)
        return "invalid quantity";

    if (error == EngineError::UnsupportedPegCombination)
        return "unsupported peg combination";

    if (error == EngineError::PegReferenceUnavailable)
        return "peg reference unavailable";

    if (error == EngineError::OrderNotFound)
        return "order not found";

    if (error == EngineError::OrderNotOpen)
        return "order is not open";

    if (error == EngineError::UnsupportedAmendment)
        return "unsupported amendment";

    return "amendment must change price, quantity, or both";
}

const char* cancellation_reason_label(CancellationReason reason) {
    if (reason == CancellationReason::UserRequested)
        return "USER_REQUESTED";

    if (reason == CancellationReason::MarketRemainder)
        return "MARKET_REMAINDER";

    return "PEG_REFERENCE_UNAVAILABLE";
}

const char* side_command_label(Side side) {
    return side == Side::Buy ? "buy" : "sell";
}

const char* peg_reference_command_label(PegReference reference) {
    return reference == PegReference::Bid ? "bid" : "offer";
}

void print_trades(
    std::ostream& output,
    const std::vector<Trade>& trades
) {
    std::size_t index = 0;

    while (index < trades.size()) {
        const Price price = trades[index].price;
        Quantity total_quantity = 0;

        do {
            total_quantity += trades[index].quantity;
            ++index;
        } while (index < trades.size() && trades[index].price == price);

        output
            << "Trade, price: "
            << format_price(price)
            << ", qty: "
            << total_quantity
            << '\n';
    }
}

void print_cancellations(
    std::ostream& output,
    const std::vector<Cancellation>& cancellations
) {
    for (const Cancellation& cancellation : cancellations) {
        output
            << "Order cancelled: id="
            << cancellation.order_id
            << ", qty="
            << cancellation.quantity
            << ", reason="
            << cancellation_reason_label(cancellation.reason)
            << '\n';
    }
}

}  // namespace

Console::Console(std::istream& input, std::ostream& output)
    : input_(input),
      output_(output) {
}

void Console::run() {
    std::string line;

    while (true) {
        output_ << "> ";
        output_.flush();

        if (!std::getline(input_, line)) {
            if (pending_limit_.has_value()) {
                pending_limit_.reset();
                output_ << "Order not submitted\n";
            }
            break;
        }

        if (!execute(line))
            break;
    }
}

bool Console::execute(const std::string& line) {
    if (pending_limit_.has_value())
        return confirm_limit(line);

    const std::vector<std::string> tokens = split(line);

    if (tokens.empty())
        return true;

    if (tokens.size() == 1 && tokens[0] == "exit") {
        output_ << "Bye\n";
        return false;
    }

    if (tokens.size() == 1 && tokens[0] == "help") {
        print_help();
        return true;
    }

    if (tokens[0] == "limit") {
        if (tokens.size() != 4) {
            output_ << "Error: usage: limit <buy|sell> <price> <quantity>\n";
            return true;
        }

        const std::optional<Side> side = parse_side(tokens[1]);
        const std::optional<Price> price = parse_price(tokens[2]);
        const std::optional<long long> quantity = parse_integer(tokens[3]);

        if (!side.has_value()) {
            output_ << "Error: invalid side\n";
            return true;
        }

        if (!price.has_value()) {
            output_ << "Error: invalid price\n";
            return true;
        }

        if (!quantity.has_value()) {
            output_ << "Error: invalid quantity\n";
            return true;
        }

        submit_limit(side.value(), price.value(), quantity.value());

        return true;
    }

    if (tokens[0] == "market") {
        if (tokens.size() != 3) {
            output_ << "Error: usage: market <buy|sell> <quantity>\n";
            return true;
        }

        const std::optional<Side> side = parse_side(tokens[1]);
        const std::optional<long long> quantity = parse_integer(tokens[2]);

        if (!side.has_value()) {
            output_ << "Error: invalid side\n";
            return true;
        }

        if (!quantity.has_value()) {
            output_ << "Error: invalid quantity\n";
            return true;
        }

        print_order_result(
            engine_.submit_market(side.value(), quantity.value()),
            ResultAction::Created
        );

        return true;
    }

    if (tokens[0] == "peg") {
        if (tokens.size() != 4) {
            output_ << "Error: usage: peg <bid|offer> <buy|sell> <quantity>\n";
            return true;
        }

        const std::optional<PegReference> reference =
            parse_peg_reference(tokens[1]);
        const std::optional<Side> side = parse_side(tokens[2]);
        const std::optional<long long> quantity = parse_integer(tokens[3]);

        if (!reference.has_value()) {
            output_ << "Error: invalid peg reference\n";
            return true;
        }

        if (!side.has_value()) {
            output_ << "Error: invalid side\n";
            return true;
        }

        if (!quantity.has_value()) {
            output_ << "Error: invalid quantity\n";
            return true;
        }

        print_order_result(
            engine_.submit_peg(
                side.value(),
                reference.value(),
                quantity.value()
            ),
            ResultAction::Created
        );

        return true;
    }

    if (tokens[0] == "cancel") {
        if (tokens.size() != 3 || tokens[1] != "order") {
            output_ << "Error: usage: cancel order <id>\n";
            return true;
        }

        const std::optional<long long> order_id = parse_integer(tokens[2]);

        if (!order_id.has_value()) {
            output_ << "Error: invalid order id\n";
            return true;
        }

        print_cancellation_result(
            engine_.cancel_order(order_id.value())
        );

        return true;
    }

    if (tokens[0] == "amend") {
        if (tokens.size() >= 2 && tokens[1] == "order") {
            if ((tokens.size() != 5 && tokens.size() != 7)) {
                output_ << "Error: usage: amend order <id> "
                        << "[price <price>] [quantity <quantity>]\n";
                return true;
            }

            const std::optional<long long> order_id =
                parse_integer(tokens[2]);

            if (!order_id.has_value()) {
                output_ << "Error: invalid order id\n";
                return true;
            }

            AmendmentRequest request;

            for (std::size_t index = 3;
                 index < tokens.size();
                 index += 2) {
                if (tokens[index] == "price" &&
                    !request.price.has_value()) {
                    const std::optional<Price> price =
                        parse_price(tokens[index + 1]);

                    if (!price.has_value()) {
                        output_ << "Error: invalid price\n";
                        return true;
                    }

                    request.price = price.value();
                    continue;
                }

                if (tokens[index] == "quantity" &&
                    !request.remaining_quantity.has_value()) {
                    const std::optional<long long> quantity =
                        parse_integer(tokens[index + 1]);

                    if (!quantity.has_value()) {
                        output_ << "Error: invalid quantity\n";
                        return true;
                    }

                    request.remaining_quantity = quantity.value();
                    continue;
                }

                output_ << "Error: invalid or repeated amendment field\n";
                return true;
            }

            print_order_result(
                engine_.amend_order(order_id.value(), request),
                ResultAction::Amended
            );

            return true;
        }

        if (tokens.size() != 4 ||
            (tokens[1] != "price" && tokens[1] != "quantity")) {
            output_ << "Error: usage: amend <price|quantity> <id> <value> "
                    << "or amend order <id> [price <price>] "
                    << "[quantity <quantity>]\n";
            return true;
        }

        const std::optional<long long> order_id = parse_integer(tokens[2]);

        if (!order_id.has_value()) {
            output_ << "Error: invalid order id\n";
            return true;
        }

        if (tokens[1] == "price") {
            const std::optional<Price> price = parse_price(tokens[3]);

            if (!price.has_value()) {
                output_ << "Error: invalid price\n";
                return true;
            }

            print_order_result(
                engine_.amend_price(order_id.value(), price.value()),
                ResultAction::Amended
            );

            return true;
        }

        const std::optional<long long> quantity = parse_integer(tokens[3]);

        if (!quantity.has_value()) {
            output_ << "Error: invalid quantity\n";
            return true;
        }

        print_order_result(
            engine_.amend_quantity(
                order_id.value(),
                quantity.value()
            ),
            ResultAction::Amended
        );

        return true;
    }

    if (tokens[0] == "print") {
        if (tokens.size() == 2 && tokens[1] == "book") {
            output_ << format_book(
                engine_.order_book().snapshot()
            );
            return true;
        }

        if (tokens.size() == 3 &&
            tokens[1] == "book" &&
            tokens[2] == "summary") {
            output_ << format_book_summary(
                engine_.order_book().snapshot()
            );
            return true;
        }

        if (tokens.size() == 4 && tokens[1] == "level") {
            const std::optional<Side> side = parse_side(tokens[2]);
            const std::optional<Price> price = parse_price(tokens[3]);

            if (!side.has_value()) {
                output_ << "Error: invalid side\n";
                return true;
            }

            if (!price.has_value() || price.value() <= 0) {
                output_ << "Error: invalid price\n";
                return true;
            }

            output_ << format_price_level(
                engine_.order_book().snapshot(),
                side.value(),
                price.value()
            );

            return true;
        }

        if (tokens.size() == 3 && tokens[1] == "order") {
            const std::optional<long long> order_id =
                parse_integer(tokens[2]);

            if (!order_id.has_value()) {
                output_ << "Error: invalid order id\n";
                return true;
            }

            const Order* order = engine_.find_order(order_id.value());

            if (order == nullptr)
                output_ << "Error: order not found\n";
            else
                output_ << format_order(*order);

            return true;
        }

        output_ << "Error: invalid print command\n";
        return true;
    }

    output_ << "Error: unknown command. Type help.\n";
    return true;
}

void Console::submit_limit(Side side, Price price, Quantity quantity) {
    // Validate before quoting: invalid input must never ask for confirmation.
    if (price <= 0) {
        print_error(EngineError::InvalidPrice);
        return;
    }
    if (quantity <= 0) {
        print_error(EngineError::InvalidQuantity);
        return;
    }

    const Side opposite = side == Side::Buy ? Side::Sell : Side::Buy;
    const Order* best = engine_.order_book().best(opposite);
    if (best != nullptr &&
        (side == Side::Buy ? price >= best->price().value()
                          : price <= best->price().value())) {
        pending_limit_ = PendingLimit{side, price, quantity};
        output_ << "Warning: limit " << side_command_label(side)
                << ' ' << quantity << " @ " << format_price(price)
                << " crosses the book.\n"
                << "Best available " << (side == Side::Buy ? "ask" : "bid")
                << ": " << format_price(best->price().value()) << ".\n"
                << "It will execute immediately, in full or in part, at available prices\n"
                << "within your limit. The best price is not guaranteed for all units.\n"
                << "Any unfilled quantity will rest at your limit price.\n"
                << "Submit this order? [y/N]\n";
        return;
    }

    print_order_result(engine_.submit_limit(side, price, quantity),
                       ResultAction::Created);
}

bool Console::confirm_limit(const std::string& line) {
    const auto tokens = split(line);
    const bool yes = tokens.size() == 1 &&
        (tokens[0] == "y" || tokens[0] == "yes" ||
         tokens[0] == "s" || tokens[0] == "sim");
    const bool stop = tokens.size() == 1 && tokens[0] == "exit";
    const bool no = tokens.empty() || stop || (tokens.size() == 1 &&
        (tokens[0] == "n" || tokens[0] == "no" ||
         tokens[0] == "nao" || tokens[0] == "não"));
    if (!yes && !no) {
        output_ << "Please answer y/yes (s/sim) or n/no. Submit? [y/N]\n";
        return true;
    }

    const PendingLimit order = pending_limit_.value();
    pending_limit_.reset();
    if (yes) {
        print_order_result(
            engine_.submit_limit(order.side, order.price, order.quantity),
            ResultAction::Created
        );
    } else {
        output_ << "Order not submitted\n";
    }
    if (stop)
        output_ << "Bye\n";
    return !stop;
}

void Console::print_help() {
    output_
        << "Commands:\n"
        << "  limit <buy|sell> <price> <quantity>\n"
        << "  market <buy|sell> <quantity>\n"
        << "  peg <bid|offer> <buy|sell> <quantity>\n"
        << "  cancel order <id>\n"
        << "  amend price <id> <price>\n"
        << "  amend quantity <id> <quantity>\n"
        << "  amend order <id> [price <price>] [quantity <quantity>]\n"
        << "  print book\n"
        << "  print book summary\n"
        << "  print level <buy|sell> <price>\n"
        << "  print order <id>\n"
        << "  help\n"
        << "  exit\n"
        << "Crossing limits require confirmation: y/yes (s/sim) to submit;\n"
        << "n/no or an empty line to discard. Amendments do not prompt.\n";
}

void Console::print_error(EngineError error) {
    output_ << "Error: " << error_message(error) << '\n';
}

void Console::print_order_result(
    const SubmissionResult& result,
    ResultAction action
) {
    const EngineError* error = std::get_if<EngineError>(&result);

    if (error != nullptr) {
        print_error(*error);
        return;
    }

    const SubmissionReport& report = std::get<SubmissionReport>(result);

    if (action == ResultAction::Amended) {
        output_ << "Order amended: id=" << report.order_id << '\n';
    } else {
        const Order* order = engine_.find_order(report.order_id);

        output_ << "Order created: ";

        if (order == nullptr) {
            output_ << "id=" << report.order_id << '\n';
        } else if (order->type() == OrderType::Market) {
            output_
                << "market "
                << side_command_label(order->side())
                << ' '
                << order->original_quantity()
                << ", id: "
                << order->id()
                << '\n';
        } else {
            if (order->is_pegged()) {
                output_
                    << "peg "
                    << peg_reference_command_label(
                        order->peg_reference().value()
                    )
                    << ' ';
            }

            output_
                << side_command_label(order->side())
                << ' '
                << order->original_quantity()
                << " @ "
                << format_price(order->price().value())
                << ", id: "
                << order->id()
                << '\n';
        }
    }

    print_trades(output_, report.trades);
    print_cancellations(output_, report.cancellations);
}

void Console::print_cancellation_result(
    const CancellationResult& result
) {
    const EngineError* error = std::get_if<EngineError>(&result);

    if (error != nullptr) {
        print_error(*error);
        return;
    }

    const CancellationReport& report =
        std::get<CancellationReport>(result);

    print_cancellations(output_, report.cancellations);
}

}  // namespace matching_engine
