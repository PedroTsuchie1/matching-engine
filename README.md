# Matching Engine
A single-asset, in-memory order matching engine written in C++20. The project is designed to support Limit, Market, and Pegged Orders while enforcing price-time priority and deterministic order processing.
## Architecture
The core is kept independent from command parsing and console output:

```text
CLI
 └── MatchingEngine
      ├── OrderBook (buy and sell sides)
      ├── Order index
      └── Pegged order management
```

- `MatchingEngine` coordinates order submission, matching, cancellation, amendment, and pegged-order repricing.
- `OrderBook` maintains ordered price levels and time priority within each level.
- `Order` is a value type created through type-specific factories, keeping order construction separate from matching behavior.
- The CLI runs until `exit` or end-of-file; `help` lists its commands and `print book` displays the current book.
- Invalid commands and operations return readable `Error: ...` messages without terminating the session.
## Tooling
- C++20
- CMake
- GoogleTest, integrated with CTest
- GCC as the reference compiler
## Build and run
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/matching_engine
```
## Matching rules

- Buy orders prioritize higher prices; sell orders prioritize lower prices.
- Orders at the same price follow arrival order.
- Priority is represented by a monotonic sequence number rather than wall-clock time.
- An aggressive Limit Order trades immediately and rests any remaining quantity at its limit price.
- A Market Order executes as much as possible immediately and never rests in the book.
- Trades execute at the price of the resting order.

## Assumptions
The current decisions are:
- **Market remainder:** Market Orders use Immediate-or-Cancel semantics; unfilled quantity is discarded.
- **Trade reporting:** Matches against individual resting orders are recorded separately. Consecutive fills at the same price may be aggregated for console output.
- **Price representation:** Prices use fixed-point integers with two decimal places. Floating-point values are not used for monetary comparisons.
- **Quantity amendment:** Reducing quantity keeps time priority; increasing it loses priority.
- **Price amendment:** Changing price loses time priority. The amended order trades immediately if its new price crosses the book and rests any remainder.
- **Supported pegs:** The initial scope supports `peg bid buy` and `peg offer sell` only.
- **Peg reference:** Pegged Orders follow prices established by regular Limit Orders. They do not establish or reference pegged prices themselves.
- **Missing peg reference:** A Pegged Order is accepted as inactive when no eligible reference exists and activates when one appears.
- **Automatic repricing:** A Pegged Order keeps its original sequence number when its reference price changes.
