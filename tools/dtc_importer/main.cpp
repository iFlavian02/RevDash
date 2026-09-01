#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include "revdash/diagnostics/dtc_database.hpp"

int main(int argc, char** argv) {
    CLI::App app{"RevDash DTC Database Importer", "revdash_dtc_importer"};

    std::string input_csv;
    std::string output_db;
    std::string import_kind;
    std::string verify_database;
    std::string verify_kind;

    auto* import_command = app.add_subcommand("import", "Validate CSV and generate a versioned SQLite database");
    import_command->add_option("-i,--input", input_csv, "Input DTC CSV file")->required();
    import_command->add_option("-o,--output", output_db, "Output SQLite database path")->required();
    import_command->add_option("--kind", import_kind, "Database identity: production or fixture")
        ->check(CLI::IsMember({"production", "fixture"}))
        ->required();

    auto* verify_command = app.add_subcommand("verify", "Verify schema and embedded database identity");
    verify_command->add_option("--database", verify_database, "SQLite database path")->required();
    verify_command->add_option("--kind", verify_kind, "Expected identity: production or fixture")
        ->check(CLI::IsMember({"production", "fixture"}))
        ->required();

    app.require_subcommand(1);

    CLI11_PARSE(app, argc, argv);

    if (*import_command) {
        const auto kind = import_kind == "production"
                              ? revdash::diagnostics::DtcDatabaseKind::Production
                              : revdash::diagnostics::DtcDatabaseKind::TestFixture;
        const auto result = revdash::diagnostics::importDtcCsv({
            .input_csv = input_csv,
            .output_database = output_db,
            .database_kind = kind});
        if (!result) {
            spdlog::error("{}: {}", result.error().code, result.error().message);
            if (!result.error().context.empty()) {
                spdlog::debug("Import context: {}", result.error().context);
            }
            return 1;
        }
        spdlog::info("Created {} DTC database at {}", import_kind, output_db);
        return 0;
    }

    const auto expected_kind = verify_kind == "production"
                                   ? revdash::diagnostics::DtcDatabaseKind::Production
                                   : revdash::diagnostics::DtcDatabaseKind::TestFixture;
    const auto database = revdash::diagnostics::DtcDatabase::openReadOnly(verify_database, expected_kind);
    if (!database) {
        spdlog::error("{}: {}", database.error().code, database.error().message);
        return 1;
    }
    spdlog::info(
        "Verified {} DTC database schema {} with source version {}",
        verify_kind,
        database->metadata().schema_version,
        database->metadata().source_version);
    return 0;
}
