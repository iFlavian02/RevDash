#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include "revdash/core/types.hpp"

int main(int argc, char** argv) {
    CLI::App app{"RevDash DTC Database Importer", "revdash_dtc_importer"};

    std::string input_csv;
    std::string output_db = "revdash_dtc.sqlite";

    app.add_option("-i,--input", input_csv, "Input licensed DTC CSV file");
    app.add_option("-o,--output", output_db, "Output SQLite database path")->default_val(output_db);

    CLI11_PARSE(app, argc, argv);

    spdlog::info("RevDash DTC Importer initialized. Target output: {}", output_db);
    return 0;
}
