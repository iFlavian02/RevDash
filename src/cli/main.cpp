#include <iostream>
#include <string>
#include <vector>

#include "revdash/cli/application.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    revdash::cli::ConsoleInterruptFlag interrupt;
    return revdash::cli::run(arguments, std::cin, std::cout, std::cerr, interrupt);
}
