#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "revdash/diagnostics/dtc_database.hpp"

using namespace revdash;

namespace {

class TestDirectory final {
public:
    TestDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("revdash-dtc-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path fixtureCsv() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "fixtures" / "dtc" / "fixture.csv";
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(static_cast<bool>(output));
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    REQUIRE(output.good());
}

[[nodiscard]] std::filesystem::path importFixture(
    const TestDirectory& directory,
    diagnostics::DtcDatabaseKind kind = diagnostics::DtcDatabaseKind::TestFixture
) {
    const auto database_path = directory.path() /
                               (kind == diagnostics::DtcDatabaseKind::TestFixture ? "fixture.sqlite" : "production.sqlite");
    const auto imported = diagnostics::importDtcCsv({
        .input_csv = fixtureCsv(),
        .output_database = database_path,
        .database_kind = kind});
    REQUIRE(imported.has_value());
    REQUIRE(std::filesystem::is_regular_file(database_path));
    return database_path;
}

[[nodiscard]] core::Result<void> importText(std::string_view text) {
    TestDirectory directory;
    const auto csv_path = directory.path() / "input.csv";
    writeFile(csv_path, text);
    return diagnostics::importDtcCsv({
        .input_csv = csv_path,
        .output_database = directory.path() / "output.sqlite",
        .database_kind = diagnostics::DtcDatabaseKind::TestFixture});
}

} // namespace

TEST_CASE("DTC fixture imports into a versioned read-only database", "[dtc_database]") {
    TestDirectory directory;
    const auto database_path = importFixture(directory);

    const auto rejected_as_production = diagnostics::DtcDatabase::openReadOnly(database_path);
    REQUIRE_FALSE(rejected_as_production.has_value());
    REQUIRE(rejected_as_production.error().code == "Diagnostics.DatabaseInvalid");

    auto opened = diagnostics::DtcDatabase::openReadOnly(
        database_path,
        diagnostics::DtcDatabaseKind::TestFixture);
    REQUIRE(opened.has_value());
    auto database = std::move(*opened);
    REQUIRE(database.isReadOnly());
    REQUIRE(database.metadata().schema_version == diagnostics::kDtcDatabaseSchemaVersion);
    REQUIRE(database.metadata().source_version == "fixture-2026.1");
    REQUIRE(database.metadata().kind == diagnostics::DtcDatabaseKind::TestFixture);
}

TEST_CASE("DTC runtime supports exact prefix keyword and unknown lookup", "[dtc_database]") {
    TestDirectory directory;
    auto opened = diagnostics::DtcDatabase::openReadOnly(
        importFixture(directory),
        diagnostics::DtcDatabaseKind::TestFixture);
    REQUIRE(opened.has_value());
    auto database = std::move(*opened);

    const auto exact = database.lookupExact("p0300");
    REQUIRE(exact.has_value());
    REQUIRE(exact->known);
    REQUIRE(exact->code == "P0300");
    REQUIRE(exact->severity == core::Severity::Critical);
    REQUIRE(exact->likely_failure_points.size() == 3);
    REQUIRE(exact->likely_failure_points.at(1) == "Ignition coils");

    const auto prefix = database.lookupPrefix("P0");
    REQUIRE(prefix.has_value());
    REQUIRE(prefix->size() == 2);
    REQUIRE(prefix->at(0).code == "P0171");
    REQUIRE(prefix->at(1).code == "P0300");

    const auto keyword = database.searchKeywords("IGNITION coil");
    REQUIRE(keyword.has_value());
    REQUIRE(keyword->size() == 1);
    REQUIRE(keyword->front().code == "P0300");

    const auto unknown = database.lookupExact("P2222");
    REQUIRE(unknown.has_value());
    REQUIRE_FALSE(unknown->known);
    REQUIRE(unknown->description == "Unknown diagnostic trouble code");
    REQUIRE(unknown->likely_failure_points.empty());
}

TEST_CASE("DTC importer rejects malformed external datasets", "[dtc_database]") {
    const std::string header = "code,description,severity,likely_failure_points,source_version\n";

    SECTION("invalid DTC code") {
        const auto result = importText(header + "X0300,Misfire,Warning,Ignition,1\n");
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == "Diagnostics.DatabaseInvalid");
    }
    SECTION("invalid severity") {
        const auto result = importText(header + "P0300,Misfire,Emergency,Ignition,1\n");
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("missing required field") {
        const auto result = importText(
            "code,description,severity,source_version\nP0300,Misfire,Warning,1\n");
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("duplicate conflict") {
        const auto result = importText(
            header + "P0300,Misfire,Warning,Ignition,1\nP0300,Other description,Critical,Fuel,1\n");
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("conflicting dataset version") {
        const auto result = importText(
            header + "P0300,Misfire,Warning,Ignition,1\nP0171,Lean,Warning,Intake,2\n");
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("invalid UTF-8") {
        auto contents = header + "P0300,Misfire,Warning,Ignition,1\n";
        contents.insert(contents.begin() + static_cast<std::ptrdiff_t>(header.size() + 7), static_cast<char>(0xC3));
        const auto result = importText(contents);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("DTC runtime rejects unsupported schema versions", "[dtc_database]") {
    TestDirectory directory;
    const auto database_path = importFixture(directory);

    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open_v2(database_path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "PRAGMA user_version=2;", nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_close(database) == SQLITE_OK);

    const auto opened = diagnostics::DtcDatabase::openReadOnly(
        database_path,
        diagnostics::DtcDatabaseKind::TestFixture);
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == "Diagnostics.DatabaseInvalid");
}

TEST_CASE("DTC fixture and production identities cannot be confused", "[dtc_database]") {
    TestDirectory directory;
    const auto fixture_path = importFixture(directory, diagnostics::DtcDatabaseKind::TestFixture);
    const auto production_path = importFixture(directory, diagnostics::DtcDatabaseKind::Production);

    REQUIRE(diagnostics::DtcDatabase::openReadOnly(
                fixture_path,
                diagnostics::DtcDatabaseKind::TestFixture).has_value());
    REQUIRE_FALSE(diagnostics::DtcDatabase::openReadOnly(fixture_path).has_value());
    REQUIRE(diagnostics::DtcDatabase::openReadOnly(production_path).has_value());
    REQUIRE_FALSE(diagnostics::DtcDatabase::openReadOnly(
                      production_path,
                      diagnostics::DtcDatabaseKind::TestFixture).has_value());
}
