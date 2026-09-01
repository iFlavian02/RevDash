#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include "revdash/core/types.hpp"

int main(int argc, char** argv) {
    CLI::App app{"RevDash OBD-II Diagnostics CLI", "revdash_cli"};

    bool version_flag = false;
    app.add_flag("-v,--version", version_flag, "Display application version");

    CLI11_PARSE(app, argc, argv);

    if (version_flag) {
        spdlog::info("{} version {}", revdash::core::kApplicationName, revdash::core::kApplicationVersion);
        return 0;
    }

    spdlog::info("Starting RevDash CLI engine...");
    return 0;
}
