#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "revdash/cli/application.hpp"

namespace {

class TestInterrupt final : public revdash::cli::InterruptFlag {
public:
    [[nodiscard]] bool requested() const noexcept override { return interrupted; }
    bool interrupted{false};
};

class ImmediateInterrupt final : public revdash::cli::InterruptFlag {
public:
    [[nodiscard]] bool requested() const noexcept override { return true; }
};

int invoke(const std::vector<std::string>& args, std::string& output, std::string& error,
           std::string input = {}) {
    std::istringstream input_stream{std::move(input)};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    TestInterrupt interrupt;
    const auto result = revdash::cli::run(args, input_stream, output_stream, error_stream, interrupt);
    output = output_stream.str();
    error = error_stream.str();
    return result;
}

} // namespace

TEST_CASE("cli parses every top-level command and uses standard usage exit", "[cli]") {
    std::string output;
    std::string error;
    CHECK(invoke({"revdash_cli", "unknown"}, output, error) == 2);
    CHECK(error.find("not expected") != std::string::npos);
    CHECK(invoke({"revdash_cli"}, output, error) == 2);
    CHECK(invoke({"revdash_cli", "--version"}, output, error) == 0);
    CHECK(output.find("RevDash version") != std::string::npos);
    CHECK(invoke({"revdash_cli", "export"}, output, error) == 2);
    CHECK(invoke({"revdash_cli", "clear"}, output, error) == 2);
}

TEST_CASE("cli sources supports human tables and JSON Lines", "[cli]") {
    std::string output;
    std::string error;
    REQUIRE(invoke({"revdash_cli", "sources"}, output, error) == 0);
    CHECK(output.find("Source") != std::string::npos);
    CHECK(output.find("synthetic") != std::string::npos);

    REQUIRE(invoke({"revdash_cli", "--jsonl", "sources"}, output, error) == 0);
    std::istringstream lines{output};
    std::string line;
    while (std::getline(lines, line)) {
        const auto value = nlohmann::json::parse(line);
        CHECK(value.at("type") == "source");
    }
}

TEST_CASE("cli runs simulated scan identify and bounded live flows", "[cli]") {
    std::string output;
    std::string error;
    REQUIRE(invoke({"revdash_cli", "--jsonl", "scan", "--misfire"}, output, error) == 0);
    CHECK(output.find("P0300") != std::string::npos);
    REQUIRE(invoke({"revdash_cli", "--jsonl", "identify", "--second-ecu"}, output, error) == 0);
    CHECK(output.find("\"type\":\"ecu\"") != std::string::npos);
    REQUIRE(invoke({"revdash_cli", "--jsonl", "live", "--duration", "0.05",
                    "--interval-ms", "10"}, output, error) == 0);
    CHECK(output.find("\"type\":\"telemetry\"") != std::string::npos);
}

TEST_CASE("cli preserves clear physical-source safety guard", "[cli]") {
    std::string output;
    std::string error;
    CHECK(invoke({"revdash_cli", "clear", "--port", "COM_DOES_NOT_EXIST"}, output, error) == 3);
    CHECK_FALSE(error.empty());
}

TEST_CASE("cli interruption abstraction stops an unbounded live command", "[cli]") {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    ImmediateInterrupt interrupt;
    CHECK(revdash::cli::run({"revdash_cli", "live", "--duration", "0"}, input, output, error,
                            interrupt) == 0);
}

TEST_CASE("cli records plays back and exports a simulated session", "[cli]") {
    const auto directory = std::filesystem::temp_directory_path() /
        ("revdash-cli-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto session = (directory / "session.jsonl").string();
    const auto csv = (directory / "session.csv").string();
    std::string output;
    std::string error;

    REQUIRE(invoke({"revdash_cli", "--jsonl", "record", session, "--duration", "0.15",
                    "--interval-ms", "20"}, output, error) == 0);
    CHECK(std::filesystem::file_size(session) > 0);
    const auto playback_result = invoke({"revdash_cli", "--jsonl", "playback", session, "--duration", "0.05",
                                         "--interval-ms", "10"}, output, error);
    INFO(error);
    REQUIRE(playback_result == 0);
    REQUIRE(invoke({"revdash_cli", "--jsonl", "export", session, csv}, output, error) == 0);
    CHECK(std::filesystem::file_size(csv) > 0);
    CHECK(invoke({"revdash_cli", "--jsonl", "export", "missing.jsonl", csv}, output, error) == 6);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
