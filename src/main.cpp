#include "matching_engine/console.hpp"

#include <iostream>

int main() {
    matching_engine::Console console(std::cin, std::cout);

    std::cout
        << "Matching Engine\n"
        << "Type help to list the available commands.\n";

    console.run();
}
