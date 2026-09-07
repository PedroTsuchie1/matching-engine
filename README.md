# Matching Engine

A single-asset, in-memory matching engine written in C++20, with limit, market,
and pegged orders. Supports price-time priority, cancellation, and amendments,
with an interactive command-line interface and an optional web interface.

## Getting started

Requires Git, a C++20 compiler, CMake 3.24 or newer, and a build tool such as Make.
The project has been tested on Ubuntu 24.04 with GCC 13 and CMake 3.28.

### Install dependencies

On Ubuntu 24.04:

```bash
sudo apt update
sudo apt install git build-essential cmake
```

CMake downloads GoogleTest 1.15.2 automatically during the first configuration,
so that step requires internet access. No separate GoogleTest installation is
needed.

### Clone the repository

```bash
git clone https://github.com/PedroTsuchie1/matching-engine.git matching-engine
cd matching-engine
```

### Build, test, and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/matching_engine
```

Tests cover order state transitions, book ordering, matching, cancellation,
amendments, pegged-order updates, formatting, and command parsing.

## Web interface

Requires Python 3.10+ and G++ with C++20. On Ubuntu, install Python with
`sudo apt install python3` if needed. From the repository root:

```bash
python3 extras/web/server.py
```

Open [localhost:8080](http://localhost:8080).

Use **Carregar demonstração** to try a sample book, or submit your own orders.
**Gerenciar ordem por ID** lets you cancel or change price and remaining quantity.
The book offers individual-order, price-level and cumulative views; **Status**
shows recent activity. The CLI and browser use separate in-memory books.

## Interactive commands

Prices must be positive decimals with at most two decimal places, using a dot
as the separator. They are stored as integer cents. Quantities must be positive
integers; quantity amendments set the **remaining quantity**, not the original
order quantity.

```text
limit <buy|sell> <price> <quantity>
market <buy|sell> <quantity>
peg <bid|offer> <buy|sell> <quantity>
cancel order <id>
amend price <id> <price>
amend quantity <id> <quantity>
amend order <id> [price <price>] [quantity <quantity>]
print book
print book summary
print level <buy|sell> <price>
print order <id>
help
exit
```

`print book` shows one order per line as `quantity @ price`, with buys and sells
side by side. Buys follow descending prices, sells ascending prices, and orders
at the same price keep their queue priority. Quantities are **not aggregated**.
`print book summary` aggregates remaining quantity and order count at each price.
Use `print order <id>` or `print level <buy|sell> <price>` for IDs, sequence and
other details.

### Confirmation for crossing limit orders

The CLI asks for confirmation before submitting a limit order that crosses the
book. The warning shows your limit and the best available opposite price: the
order will execute immediately, fully or partially. Fills use resting prices
within your limit and may span multiple levels. Any remainder rests at your
limit price.

Reply `y`/`yes` to submit or `n`/`no` to discard. For piped input, include the
response on its own line. Confirmation applies only to new crossing limit
submissions in the CLI, not amendments or direct calls to the matching core.

## Architecture

The matching core is independent from command parsing and console output.

| Component | Responsibility |
| --- | --- |
| [MatchingEngine](include/matching_engine/matching_engine.hpp) | Owns orders and coordinates matching, cancellation, amendments, and peg updates. |
| [OrderBook](include/matching_engine/order_book.hpp) | Indexes active orders by price and sequence, and provides book snapshots. |
| [Order](include/matching_engine/order.hpp) | Holds order state and applies fills, amendments, repricing, and cancellation. |
| [Console](include/matching_engine/console.hpp) | Parses commands, confirms crossing limit submissions, and reports results. |
| [Formatting](include/matching_engine/formatting.hpp) | Formats prices, orders, and book snapshots for display. |

## Design decisions

- Once submitted, crossing limits execute immediately to use available liquidity; any remainder rests.
- Price changes and quantity increases lose priority; quantity reductions keep it.
- Pegs support `bid buy` and `offer sell`, following regular limits only. They
  reprice after each operation's matching completes, preserving priority.
  Without a reference, new pegs are rejected and resting pegs are cancelled.
