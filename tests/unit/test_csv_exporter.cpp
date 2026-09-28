#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "revdash/session/csv_exporter.hpp"
#include "revdash/session/session_recorder.hpp"

namespace {

using namespace revdash;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("revdash-csv-" + session::generateSessionUuid());
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

void writeLine(std::ofstream& output, nlohmann::json value) {
    value["schema_version"] = 1;
    output << value.dump() << '\n';
}

void writeSession(
    const std::filesystem::path& path,
    std::int64_t duration_us,
    bool include_invalid_sample = false) {
    std::ofstream output{path, std::ios::binary};
    writeLine(output, {{"type", "header"}, {"elapsed_us", 0},
                       {"adapter_metadata", {{"name", "Adapter, \"quoted\""}}}});
    writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 0}, {"metric", "RPM"},
                       {"quality", "Valid"}, {"value", 1000.0}});
    writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 0}, {"metric", "VehicleSpeed"},
                       {"quality", "Valid"}, {"value", 100.0}});
    writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 0}, {"metric", "CoolantTemp"},
                       {"quality", "Valid"}, {"value", 0.0}});
    writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 100'000}, {"metric", "RPM"},
                       {"quality", "Valid"}, {"value", 1100.0}});
    writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 100'000}, {"metric", "MAF"},
                       {"quality", "Valid"}, {"value", 10.0}});
    if (include_invalid_sample) {
        writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 600'000}, {"metric", "VehicleSpeed"},
                           {"quality", "Dropped"}, {"value", 0.0}});
    }
    writeLine(output, {{"type", "footer"}, {"elapsed_us", duration_us}});
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string readFixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path().parent_path() / "fixtures" / "csv" / name;
    auto contents = readFile(path);
    contents.erase(std::remove(contents.begin(), contents.end(), '\r'), contents.end());
    return contents;
}

} // namespace

TEST_CASE("csv_export matches golden files for every preset", "[csv_export]") {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source, quoted.jsonl";
    writeSession(source, 200'000);

    struct Case {
        session::CsvPreset preset;
        session::UnitSystem units;
        std::string_view fixture;
    };
    constexpr Case cases[]{
        {session::CsvPreset::RevDash, session::UnitSystem::Metric, "revdash.csv"},
        {session::CsvPreset::MegaLogViewer, session::UnitSystem::Imperial, "megalogviewer.csv"},
        {session::CsvPreset::TunerStudio, session::UnitSystem::Metric, "tunerstudio.csv"}
    };
    for (const auto& test_case : cases) {
        const auto destination = directory.path() / std::string{test_case.fixture};
        INFO("Preset: " << session::toString(test_case.preset));
        REQUIRE(session::exportSessionCsv(source, destination,
            {.preset = test_case.preset, .units = test_case.units}));
        CHECK(readFile(destination) == readFixture(test_case.fixture));
    }
}

TEST_CASE("csv_export uses an exact configurable timeline and bounded sample hold", "[csv_export]") {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.jsonl";
    const auto destination = directory.path() / "export.csv";
    writeSession(source, 1'500'000, true);

    REQUIRE(session::exportSessionCsv(source, destination,
        {.preset = session::CsvPreset::MegaLogViewer,
         .units = session::UnitSystem::Metric,
         .interval = std::chrono::milliseconds{500}}));
    std::istringstream input{readFile(destination)};
    std::string header;
    std::string at_zero;
    std::string at_half;
    std::string at_one;
    std::string at_end;
    REQUIRE(std::getline(input, header));
    REQUIRE(std::getline(input, at_zero));
    REQUIRE(std::getline(input, at_half));
    REQUIRE(std::getline(input, at_one));
    REQUIRE(std::getline(input, at_end));
    CHECK(at_zero.starts_with("0,1000,100,"));
    CHECK(at_half.starts_with("0.5,1100,100,"));
    CHECK(at_one.starts_with("1,1100,,"));
    CHECK(at_end.starts_with("1.5,,,"));
    std::string extra;
    CHECK_FALSE(std::getline(input, extra));
}

TEST_CASE("csv_export is locale independent, escapes headers, and replaces atomically", "[csv_export]") {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.jsonl";
    const auto destination = directory.path() / "export.csv";
    writeSession(source, 100'000);
    {
        std::ofstream existing{destination, std::ios::binary};
        existing << "old contents";
    }

    const auto export_result = session::exportSessionCsv(source, destination,
        {.preset = session::CsvPreset::RevDash, .units = session::UnitSystem::Imperial});
    const auto export_detail = export_result.has_value() ? std::string{} : export_result.error().context;
    INFO(export_detail);
    REQUIRE(export_result);
    const auto exported = readFile(destination);
    CHECK(exported.find("old contents") == std::string::npos);
    CHECK(exported.find("# Export Preset,RevDash\n") == 0);
    CHECK(exported.find("VehicleSpeed [mph]") != std::string::npos);
    CHECK(exported.find("62.1371192237") != std::string::npos);
    CHECK(exported.find("32") != std::string::npos);
    CHECK(exported.find("62,137") == std::string::npos);

    const auto before_failure = exported;
    const auto failed = session::exportSessionCsv(source, directory.path() / "missing" / "export.csv");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "Storage.Unavailable");
    CHECK(readFile(destination) == before_failure);
}

TEST_CASE("csv_export rejects invalid options and incomplete or malformed sessions", "[csv_export]") {
    TemporaryDirectory directory;
    const auto valid = directory.path() / "valid.jsonl";
    writeSession(valid, 0);
    CHECK_FALSE(session::exportSessionCsv(valid, valid));
    CHECK_FALSE(session::exportSessionCsv(valid, directory.path() / "zero.csv",
        {.interval = std::chrono::microseconds{0}}));

    const auto incomplete = directory.path() / "incomplete.jsonl";
    {
        std::ofstream output{incomplete, std::ios::binary};
        writeLine(output, {{"type", "header"}, {"elapsed_us", 0}});
    }
    const auto incomplete_result = session::exportSessionCsv(incomplete, directory.path() / "incomplete.csv");
    REQUIRE_FALSE(incomplete_result);
    CHECK(incomplete_result.error().code == "Session.InvalidFormat");

    const auto malformed = directory.path() / "malformed.jsonl";
    {
        std::ofstream output{malformed, std::ios::binary};
        writeLine(output, {{"type", "header"}, {"elapsed_us", 0}});
        writeLine(output, {{"type", "telemetry"}, {"elapsed_us", 1}, {"metric", "NotAMetric"},
                           {"quality", "Valid"}, {"value", 1.0}});
        writeLine(output, {{"type", "footer"}, {"elapsed_us", 2}});
    }
    CHECK_FALSE(session::exportSessionCsv(malformed, directory.path() / "malformed.csv"));
}
