#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "revdash/session/session_recorder.hpp"

namespace {

using namespace revdash;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("revdash-session-" + session::generateSessionUuid());
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) result.push_back(std::move(line));
    return result;
}

[[nodiscard]] session::SessionHeader header() {
    return session::SessionHeader{
        .uuid = "00112233-4455-4677-8899-AABBCCDDEEFF",
        .application_version = "0.1.0",
        .schema_version = 1,
        .utc_start = core::UtcTimePoint{std::chrono::milliseconds{1'704'067'200'123LL}},
        .source_type = core::DataSourceType::Synthetic,
        .adapter_metadata = {{"name", "Simulátor Δ"}},
        .protocol_metadata = {{"protocol", "ISO 15765-4 CAN"}},
        .vehicle_metadata = {{.ecu_address = core::EcuAddress{0x7E8}, .vin = "1M8GDM9AXKP042788", .calibration_ids = {"CAL-α"}, .cvns = {"0123ABCD"}, .protocol_name = "CAN"}},
        .simulation = session::SimulationMetadata{.seed = 42, .displacement_liters = 2.0, .cylinder_count = 4, .initial_rpm = 800.0, .ambient_temp_c = 20.0}};
}

class MemoryStorage final : public session::ISessionStorage {
public:
    core::Result<void> open(const std::filesystem::path&) override { opened = true; return core::makeSuccess(); }
    core::Result<void> write(std::string_view bytes) override {
        ++writes;
        if (fail_on_write != 0 && writes == fail_on_write) {
            return core::makeError(core::ErrorCode::StorageUnavailable, "Injected write failure");
        }
        data.append(bytes);
        return core::makeSuccess();
    }
    core::Result<void> flush() override { flushed = true; return core::makeSuccess(); }
    core::Result<void> close() override { closed = true; return core::makeSuccess(); }
    core::Result<void> publish(const std::filesystem::path&, const std::filesystem::path&) override {
        published = true;
        return core::makeSuccess();
    }
    std::string data;
    std::size_t writes{0};
    std::size_t fail_on_write{0};
    bool opened{false};
    bool flushed{false};
    bool closed{false};
    bool published{false};
};

} // namespace

TEST_CASE("session_recorder writes and publishes every Schema v1 record type", "[session_recorder]") {
    TemporaryDirectory directory;
    const auto output = directory.path() / "complete.jsonl";
    const auto start = core::MonotonicTimePoint{std::chrono::seconds{10}};
    session::JsonlSessionRecorder recorder;
    REQUIRE(recorder.start(output, header(), start));

    const std::array<std::uint8_t, 4> payload{0x41, 0x0C, 0x1A, 0xF8};
    auto message = core::ObdMessage::create(core::DataSourceType::Synthetic, core::EcuAddress{0x7E8}, payload, 7,
                                            start + std::chrono::microseconds{1234},
                                            core::UtcTimePoint{std::chrono::milliseconds{1'704'067'200'124LL}});
    REQUIRE(message);
    REQUIRE(recorder.record(*message));

    core::TelemetrySample sample{.metric_id = core::MetricId::Rpm, .value = 1726.0,
        .quality = core::SampleQuality::Valid, .monotonic_ts = start + std::chrono::microseconds{1234},
        .utc_ts = std::nullopt, .sequence_number = 7, .ecu_address = core::EcuAddress{0x7E8}};
    REQUIRE(recorder.record(sample));
    core::DtcRecord dtc{.code = "P0300", .status = core::DtcStatus::Confirmed, .severity = core::Severity::Warning,
        .description = "Random misfire", .likely_failure_points = {"Ignition"}, .ecu_address = core::EcuAddress{0x7E8}, .freeze_frame = std::nullopt};
    REQUIRE(recorder.record(dtc, start + std::chrono::microseconds{2000}));
    core::DiagnosticFinding finding{.rule_id = "charging.low", .rule_version = "1.2", .severity = core::Severity::Warning,
        .title = "Încărcare joasă", .description = "Voltage low", .evidence = {"11.8 V"},
        .first_detected = start, .last_seen = start + std::chrono::microseconds{2000},
        .last_evaluated = start + std::chrono::microseconds{2000}, .resolved_at = std::nullopt, .active = true};
    REQUIRE(recorder.record(finding, start + std::chrono::microseconds{2000}));
    core::Mode04AuditRecord audit{.prepared_at = start, .completed_at = start + std::chrono::microseconds{3000},
        .completed_utc = std::nullopt, .source_type = core::DataSourceType::SerialElm327, .engine_epoch = 2,
        .vehicle_identity = "1M8GDM9AXKP042788", .warning = std::string{core::kClearDiagnosticWarning},
        .preparation_snapshot = {}, .post_clear_dtcs = {}, .request_transmitted = true,
        .positive_response = true, .post_clear_rescan_completed = true, .error = std::nullopt};
    audit.preparation_snapshot.captured_at = start;
    REQUIRE(recorder.record(audit));
    core::EcuMetadata metadata{.ecu_address = core::EcuAddress{0x18DAF110, core::EcuAddressFormat::Can29Bit},
        .vin = "1M8GDM9AXKP042788", .calibration_ids = {"CAL1"}, .cvns = {"DEADBEEF"}, .protocol_name = "CAN"};
    REQUIRE(recorder.record(metadata, start + std::chrono::microseconds{4000}));
    REQUIRE(recorder.observeQueueDrops(3, start + std::chrono::microseconds{5000}));
    REQUIRE(recorder.observeQueueDrops(3, start + std::chrono::microseconds{6000}));
    REQUIRE(recorder.observeQueueDrops(5, start + std::chrono::microseconds{7000}));
    REQUIRE(recorder.finish(start + std::chrono::microseconds{8000}));

    REQUIRE(std::filesystem::is_regular_file(output));
    REQUIRE_FALSE(std::filesystem::exists(session::partialSessionPath(output)));
    const auto records = lines(output);
    REQUIRE(records.size() == 10);
    const std::array expected_types{"header", "obd_message", "telemetry", "dtc", "diagnostic_finding",
                                    "mode04_audit", "ecu_metadata", "data_loss", "data_loss", "footer"};
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto parsed = session::parseSessionRecord(records[index]);
        REQUIRE(parsed);
        CHECK(parsed->at("type") == expected_types[index]);
        CHECK(parsed->at("schema_version") == 1);
        CHECK(parsed->at("elapsed_us").is_number_integer());
    }
    CHECK(records.front().find("Simulátor Δ") != std::string::npos);
    CHECK(nlohmann::json::parse(records[1]).at("payload_hex") == "410C1AF8");
    CHECK(nlohmann::json::parse(records[1]).at("ecu").at("value") == 0x7E8);
    const auto footer = nlohmann::json::parse(records.back());
    CHECK(footer.at("statistics").at("dropped_records") == 5);
    CHECK(footer.at("statistics").at("total_records") == 10);
}

TEST_CASE("session_recorder serialization is deterministic and timestamps stay integral", "[session_recorder]") {
    auto storage = std::make_unique<MemoryStorage>();
    auto* observed = storage.get();
    session::JsonlSessionRecorder recorder{std::move(storage)};
    const auto start = core::MonotonicTimePoint{std::chrono::seconds{5}};
    REQUIRE(recorder.start("fixture.jsonl", header(), start));
    const std::array<std::uint8_t, 2> payload{0x41, 0x0D};
    const auto message = core::ObdMessage::create(core::DataSourceType::Synthetic, std::nullopt, payload, 9,
        start + std::chrono::microseconds{17}, std::nullopt);
    REQUIRE(message);
    REQUIRE(recorder.record(*message));
    REQUIRE(recorder.finish(start + std::chrono::microseconds{18}));
    REQUIRE(observed->flushed);
    REQUIRE(observed->closed);
    REQUIRE(observed->published);
    std::istringstream input{observed->data};
    std::string header_line;
    std::string message_line;
    std::getline(input, header_line);
    std::getline(input, message_line);
    CHECK(header_line == "{\"adapter_metadata\":{\"name\":\"Simulátor Δ\"},\"application_version\":\"0.1.0\",\"elapsed_us\":0,\"protocol_metadata\":{\"protocol\":\"ISO 15765-4 CAN\"},\"schema_version\":1,\"simulation\":{\"ambient_temp_c\":20.0,\"cylinder_count\":4,\"displacement_liters\":2.0,\"initial_rpm\":800.0,\"seed\":42},\"source_type\":\"Synthetic\",\"type\":\"header\",\"utc_start\":\"2024-01-01T00:00:00.123Z\",\"uuid\":\"00112233-4455-4677-8899-AABBCCDDEEFF\",\"vehicle_metadata\":[{\"calibration_ids\":[\"CAL-α\"],\"cvns\":[\"0123ABCD\"],\"ecu\":{\"format\":\"can_11_bit\",\"value\":2024},\"protocol_name\":\"CAN\",\"vin\":\"1M8GDM9AXKP042788\"}]}");
    CHECK(message_line == "{\"ecu\":null,\"elapsed_us\":17,\"payload_hex\":\"410D\",\"schema_version\":1,\"sequence\":9,\"source_type\":\"Synthetic\",\"type\":\"obd_message\"}");
}

TEST_CASE("session_recorder leaves partial files and reports path and write failures", "[session_recorder]") {
    TemporaryDirectory directory;
    const auto interrupted_output = directory.path() / "interrupted.jsonl";
    {
        session::JsonlSessionRecorder recorder;
        REQUIRE(recorder.start(interrupted_output, header(), core::MonotonicTimePoint{}));
    }
    REQUIRE(std::filesystem::is_regular_file(session::partialSessionPath(interrupted_output)));
    REQUIRE_FALSE(std::filesystem::exists(interrupted_output));

    session::JsonlSessionRecorder invalid;
    const auto unavailable = directory.path() / "missing" / "recording.jsonl";
    const auto invalid_result = invalid.start(unavailable, header(), core::MonotonicTimePoint{});
    REQUIRE_FALSE(invalid_result);
    CHECK(invalid_result.error().code == "Storage.Unavailable");

    auto storage = std::make_unique<MemoryStorage>();
    storage->fail_on_write = 2;
    auto* observed = storage.get();
    session::JsonlSessionRecorder failing{std::move(storage)};
    REQUIRE(failing.start("failure.jsonl", header(), core::MonotonicTimePoint{}));
    core::TelemetrySample sample{.metric_id = core::MetricId::Rpm, .value = 800.0,
        .quality = core::SampleQuality::Valid, .monotonic_ts = core::MonotonicTimePoint{},
        .utc_ts = std::nullopt, .sequence_number = 1, .ecu_address = std::nullopt};
    const auto write_result = failing.record(sample);
    REQUIRE_FALSE(write_result);
    CHECK(write_result.error().code == "Storage.Unavailable");
    CHECK(observed->closed);
    CHECK_FALSE(observed->published);
}

TEST_CASE("session_recorder rejects malformed records and generates version-4 UUIDs", "[session_recorder]") {
    REQUIRE_FALSE(session::parseSessionRecord("not json"));
    REQUIRE_FALSE(session::parseSessionRecord(R"({"schema_version":2,"type":"header","elapsed_us":0})"));
    REQUIRE_FALSE(session::parseSessionRecord(R"({"schema_version":1,"type":"invented","elapsed_us":0})"));
    REQUIRE_FALSE(session::parseSessionRecord(R"({"schema_version":1,"type":"header","elapsed_us":-1})"));
    const auto uuid = session::generateSessionUuid();
    REQUIRE(uuid.size() == 36);
    CHECK(uuid[14] == '4');
    CHECK((uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'A' || uuid[19] == 'B'));
}

TEST_CASE("session_recorder steady telemetry keeps its preallocated output buffer", "[session_recorder]") {
    auto storage = std::make_unique<MemoryStorage>();
    session::JsonlSessionRecorder recorder{std::move(storage)};
    const auto start = core::MonotonicTimePoint{};
    REQUIRE(recorder.start("allocation-check.jsonl", header(), start));
    for (std::uint64_t sequence = 0; sequence < 1'000; ++sequence) {
        const core::TelemetrySample sample{.metric_id = core::MetricId::Rpm, .value = 800.0 + static_cast<double>(sequence),
            .quality = core::SampleQuality::Valid, .monotonic_ts = start + std::chrono::microseconds{sequence},
            .utc_ts = std::nullopt, .sequence_number = sequence, .ecu_address = core::EcuAddress{0x7E8}};
        REQUIRE(recorder.record(sample));
    }
    CHECK(recorder.statistics().serialization_buffer_growths == 0);
    REQUIRE(recorder.finish(start + std::chrono::milliseconds{1}));
}
