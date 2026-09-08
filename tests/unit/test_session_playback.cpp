#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "revdash/core/engine_service.hpp"
#include "revdash/drivers/playback.hpp"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

class TempDirectory {
public:
    TempDirectory() : path_(std::filesystem::temp_directory_path() /
        ("revdash-playback-" + revdash::session::generateSessionUuid())) {
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

Json header() {
    return Json{{"schema_version", 1}, {"type", "header"}, {"elapsed_us", 0},
                {"uuid", "12345678-1234-4234-8234-123456789ABC"}, {"application_version", "test"},
                {"utc_start", "2026-01-01T00:00:00.000Z"}, {"source_type", "Synthetic"},
                {"adapter_metadata", Json::object()}, {"protocol_metadata", Json::object()},
                {"vehicle_metadata", Json::array()}, {"simulation", nullptr}};
}

Json obd(std::int64_t elapsed, std::uint64_t sequence, std::string payload) {
    return Json{{"schema_version", 1}, {"type", "obd_message"}, {"elapsed_us", elapsed},
                {"source_type", "Synthetic"}, {"sequence", sequence},
                {"ecu", {{"format", "can_11_bit"}, {"value", 0x7E8}}},
                {"payload_hex", std::move(payload)}};
}

Json finding(std::int64_t elapsed) {
    return Json{{"schema_version", 1}, {"type", "diagnostic_finding"}, {"elapsed_us", elapsed},
                {"rule_id", "fixture"}, {"rule_version", "1.0"}, {"severity", "Warning"},
                {"title", "Recorded"}, {"description", "Historical only"}, {"evidence", Json::array()},
                {"first_detected_elapsed_us", elapsed}, {"last_seen_elapsed_us", elapsed},
                {"last_evaluated_elapsed_us", elapsed}, {"resolved_elapsed_us", nullptr}, {"active", true}};
}

Json audit(std::int64_t elapsed) {
    return Json{{"schema_version", 1}, {"type", "mode04_audit"}, {"elapsed_us", elapsed},
                {"prepared_elapsed_us", elapsed}, {"completed_elapsed_us", elapsed},
                {"source_type", "SerialElm327"}, {"engine_epoch", 1}, {"vehicle_identity", nullptr},
                {"warning", "warning"}, {"preparation_snapshot", Json::object()},
                {"post_clear_dtcs", Json::array()}, {"request_transmitted", true},
                {"positive_response", true}, {"post_clear_rescan_completed", true}, {"error", nullptr}};
}

Json footer(std::int64_t elapsed) {
    return Json{{"schema_version", 1}, {"type", "footer"}, {"elapsed_us", elapsed},
                {"statistics", {{"obd_messages", 0}, {"telemetry_samples", 0}, {"dtcs", 0},
                                {"diagnostic_findings", 0}, {"mode04_audits", 0}, {"ecu_metadata", 0},
                                {"data_loss_markers", 0}, {"dropped_records", 0},
                                {"serialization_buffer_growths", 0}, {"total_records", 0}}}};
}

void writeLines(const std::filesystem::path& path, const std::vector<Json>& records) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    for (const auto& record : records) output << record.dump() << '\n';
}

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

revdash::core::Result<void> connect(
    revdash::drivers::PlaybackDataSource& source,
    const std::filesystem::path& path,
    double speed = 1.0) {
    std::atomic<bool> done{false};
    revdash::core::Result<void> result = revdash::core::makeSuccess();
    source.connect(revdash::core::PlaybackConfig{
        .session_file_path = path.string(), .speed_multiplier = speed, .loop = false},
        [&](auto completed) { result = std::move(completed); done = true; });
    REQUIRE(waitFor([&] { return done.load(); }));
    return result;
}

std::filesystem::path validSession(const TempDirectory& directory) {
    const auto path = directory.path() / "fixture.jsonl";
    writeLines(path, {header(), obd(0, 1, "410D0A"), obd(1'000'000, 2, "410D14"),
                      finding(1'500'000), audit(1'750'000), obd(2'000'000, 3, "410D1E"), footer(2'000'000)});
    return path;
}

std::filesystem::path chargingSession(const TempDirectory& directory) {
    const auto path = directory.path() / "charging.jsonl";
    std::vector<Json> records{header()};
    std::uint64_t sequence = 1;
    for (std::int64_t second = 0; second <= 20; ++second) {
        const auto elapsed = second * 1'000'000;
        records.push_back(obd(elapsed, sequence++, "410C1770")); // 1500 rpm
        records.push_back(obd(elapsed, sequence++, "410D28"));   // 40 km/h
        records.push_back(obd(elapsed, sequence++, "410459"));   // approx. 35% load
        records.push_back(obd(elapsed, sequence++, "410578"));   // 80 C
        records.push_back(obd(elapsed, sequence++, "410780"));   // 0% LTFT
        records.push_back(obd(elapsed, sequence++, "41463C"));   // 20 C ambient
        records.push_back(obd(elapsed, sequence++, "41422710")); // 10 V
    }
    records.push_back(footer(20'000'000));
    writeLines(path, records);
    return path;
}

} // namespace

TEST_CASE("Playback validates sessions and creates fingerprinted seek indexes", "[session_playback]") {
    TempDirectory directory;
    const auto path = validSession(directory);
    revdash::drivers::PlaybackDataSource source;
    REQUIRE(connect(source, path));
    REQUIRE(source.indexWasRebuilt());
    auto sidecar = path; sidecar.replace_extension(".ridx");
    REQUIRE(std::filesystem::is_regular_file(sidecar));
    REQUIRE(source.duration() == 2s);
    REQUIRE(source.historicalFindings().size() == 1);
    REQUIRE(source.historicalMode04Audits().size() == 1);

    revdash::drivers::PlaybackDataSource cached;
    REQUIRE(connect(cached, path));
    REQUIRE_FALSE(cached.indexWasRebuilt());

    {
        std::ofstream stale(sidecar, std::ios::binary | std::ios::trunc);
        stale << R"({"index_version":1,"schema_version":1,"source_size":0,"source_hash":0})";
    }
    revdash::drivers::PlaybackDataSource rebuilt;
    REQUIRE(connect(rebuilt, path));
    REQUIRE(rebuilt.indexWasRebuilt());
}

TEST_CASE("Playback timing, pause, step, and supported multipliers are deterministic", "[session_playback]") {
    TempDirectory directory;
    const auto path = validSession(directory);
    auto clock = std::make_shared<revdash::core::ManualClock>();
    revdash::drivers::PlaybackDataSource source(clock);
    REQUIRE(connect(source, path, 2.0));
    std::mutex mutex;
    std::vector<revdash::core::ObdMessage> messages;
    auto subscription = source.subscribe([&](const auto& message) { std::lock_guard lock(mutex); messages.push_back(message); }, {});

    std::atomic<bool> played{false};
    source.play([&](auto result) { REQUIRE(result); played = true; });
    REQUIRE(waitFor([&] { return played.load(); }));
    REQUIRE(waitFor([&] { std::lock_guard lock(mutex); return messages.size() == 1; }));
    clock->advance(500ms);
    source.poll();
    REQUIRE(waitFor([&] { std::lock_guard lock(mutex); return messages.size() == 2; }));

    std::atomic<bool> paused{false};
    source.pause([&](auto result) { REQUIRE(result); paused = true; });
    REQUIRE(waitFor([&] { return paused.load(); }));
    clock->advance(5s);
    source.poll();
    std::this_thread::yield();
    { std::lock_guard lock(mutex); REQUIRE(messages.size() == 2); }

    std::atomic<bool> resumed{false};
    source.play([&](auto result) { REQUIRE(result); resumed = true; });
    REQUIRE(waitFor([&] { return resumed.load(); }));
    clock->advance(500ms);
    source.poll();
    REQUIRE(waitFor([&] { std::lock_guard lock(mutex); return messages.size() == 3; }));

    std::atomic<bool> stopped{false};
    source.stop([&](auto result) { REQUIRE(result); stopped = true; });
    REQUIRE(waitFor([&] { return stopped.load(); }));
    REQUIRE(source.playbackState() == revdash::drivers::PlaybackState::Stopped);
    std::atomic<bool> stepped{false};
    source.step([&](auto result) { REQUIRE(result); stepped = true; });
    REQUIRE(waitFor([&] { return stepped.load(); }));
    REQUIRE(waitFor([&] { std::lock_guard lock(mutex); return messages.size() == 4; }));
    REQUIRE(source.playbackState() == revdash::drivers::PlaybackState::Paused);

    for (const double multiplier : {0.5, 1.0, 2.0, 5.0}) {
        std::atomic<bool> changed{false};
        source.setSpeedMultiplier(multiplier, [&](auto result) { REQUIRE(result); changed = true; });
        REQUIRE(waitFor([&] { return changed.load(); }));
    }
    std::atomic<bool> rejected{false};
    source.setSpeedMultiplier(3.0, [&](auto result) { REQUIRE_FALSE(result); rejected = true; });
    REQUIRE(waitFor([&] { return rejected.load(); }));
}

TEST_CASE("Every supported playback multiplier scales delivery time", "[session_playback]") {
    TempDirectory directory;
    const auto path = validSession(directory);
    for (const double multiplier : {0.5, 1.0, 2.0, 5.0}) {
        auto clock = std::make_shared<revdash::core::ManualClock>();
        revdash::drivers::PlaybackDataSource source(clock);
        REQUIRE(connect(source, path, multiplier));
        std::atomic<int> received{0};
        auto subscription = source.subscribe([&](const auto&) { ++received; }, {});
        std::atomic<bool> played{false};
        source.play([&](auto result) { REQUIRE(result); played = true; });
        REQUIRE(waitFor([&] { return played.load() && received.load() == 1; }));
        const auto delivery = std::chrono::duration_cast<std::chrono::microseconds>(1s / multiplier);
        clock->advance(delivery - 1us);
        source.poll();
        std::this_thread::yield();
        REQUIRE(received.load() == 1);
        clock->advance(1us);
        source.poll();
        REQUIRE(waitFor([&] { return received.load() == 2; }));
    }
}

TEST_CASE("Playback seek fast-forwards its warmup range and blocks Mode 04", "[session_playback]") {
    TempDirectory directory;
    const auto path = validSession(directory);
    revdash::drivers::PlaybackDataSource source;
    REQUIRE(connect(source, path));
    std::mutex mutex;
    std::vector<std::uint64_t> sequences;
    auto subscription = source.subscribe([&](const auto& message) { std::lock_guard lock(mutex); sequences.push_back(message.sequence_number); }, {});

    std::atomic<bool> sought{false};
    source.seek(2s, 1s, [&](auto result) { REQUIRE(result); sought = true; });
    REQUIRE(waitFor([&] { return sought.load(); }));
    { std::lock_guard lock(mutex); REQUIRE(sequences == std::vector<std::uint64_t>{2, 3}); }
    REQUIRE(source.position() == 2s);

    std::atomic<bool> rejected{false};
    source.transmit(revdash::core::ObdRequest{.mode = 0x04}, [&](auto result) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == "Diagnostics.Unsupported");
        rejected = true;
    });
    REQUIRE(waitFor([&] { return rejected.load(); }));
}

TEST_CASE("Playback rejects corrupt, incompatible, non-monotonic, and truncated sessions", "[session_playback]") {
    TempDirectory directory;

    const auto corrupt = directory.path() / "corrupt.jsonl";
    { std::ofstream output(corrupt); output << "not json\n"; }
    revdash::drivers::PlaybackDataSource corrupt_source;
    REQUIRE_FALSE(connect(corrupt_source, corrupt));

    auto incompatible_header = header(); incompatible_header["schema_version"] = 2;
    const auto incompatible = directory.path() / "incompatible.jsonl";
    writeLines(incompatible, {incompatible_header, footer(0)});
    revdash::drivers::PlaybackDataSource incompatible_source;
    REQUIRE_FALSE(connect(incompatible_source, incompatible));

    const auto non_monotonic = directory.path() / "non-monotonic.jsonl";
    writeLines(non_monotonic, {header(), obd(2, 1, "410D01"), obd(1, 2, "410D02"), footer(2)});
    revdash::drivers::PlaybackDataSource non_monotonic_source;
    REQUIRE_FALSE(connect(non_monotonic_source, non_monotonic));

    const auto truncated = directory.path() / "truncated.jsonl";
    writeLines(truncated, {header(), obd(0, 1, "410D01")});
    revdash::drivers::PlaybackDataSource truncated_source;
    REQUIRE_FALSE(connect(truncated_source, truncated));

    const auto bad_hex = directory.path() / "bad-hex.jsonl";
    writeLines(bad_hex, {header(), obd(0, 1, "41ff"), footer(0)});
    revdash::drivers::PlaybackDataSource bad_hex_source;
    REQUIRE_FALSE(connect(bad_hex_source, bad_hex));
}

TEST_CASE("Engine seek resets its epoch and rebuilds telemetry through the normal decoder", "[session_playback]") {
    TempDirectory directory;
    const auto path = validSession(directory);
    revdash::core::EngineService engine;
    std::atomic<bool> installed{false};
    engine.setSource(std::make_unique<revdash::drivers::PlaybackDataSource>(), [&](auto result) { REQUIRE(result); installed = true; });
    REQUIRE(waitFor([&] { return installed.load(); }));
    std::atomic<bool> connected{false};
    engine.connect(revdash::core::PlaybackConfig{.session_file_path = path.string()}, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    const auto before = engine.epoch();
    std::atomic<bool> sought{false};
    engine.seekPlayback(2s, [&](auto result) { REQUIRE(result); sought = true; });
    REQUIRE(waitFor([&] { return sought.load(); }));
    REQUIRE(engine.epoch() == before + 1);
    REQUIRE(engine.telemetrySnapshot().get(revdash::core::MetricId::VehicleSpeed).value == 30.0);
    REQUIRE(engine.historicalPlaybackFindings().size() == 1);
    REQUIRE(engine.historicalPlaybackMode04Audits().size() == 1);
}

TEST_CASE("Engine seek derives enough warmup to re-evaluate current diagnostic rules", "[session_playback]") {
    TempDirectory directory;
    const auto path = chargingSession(directory);
    revdash::core::EngineService engine;
    std::atomic<bool> installed{false};
    engine.setSource(std::make_unique<revdash::drivers::PlaybackDataSource>(), [&](auto result) { REQUIRE(result); installed = true; });
    REQUIRE(waitFor([&] { return installed.load(); }));
    std::atomic<bool> connected{false};
    engine.connect(revdash::core::PlaybackConfig{.session_file_path = path.string()}, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    std::atomic<bool> sought{false};
    engine.seekPlayback(20s, [&](auto result) { REQUIRE(result); sought = true; });
    REQUIRE(waitFor([&] { return sought.load(); }));
    REQUIRE(engine.telemetrySnapshot().get(revdash::core::MetricId::Rpm).value == 1500.0);
    REQUIRE(engine.telemetrySnapshot().get(revdash::core::MetricId::CoolantTemp).value == 80.0);
    REQUIRE(engine.telemetrySnapshot().get(revdash::core::MetricId::ModuleVoltage).value == 10.0);
    REQUIRE(engine.sourceQueueHealth().pushed == 147);
    REQUIRE(engine.sourceQueueHealth().popped == 147);
    const auto findings = engine.diagnosticFindings();
    REQUIRE(std::ranges::any_of(findings, [](const auto& item) {
        return item.rule_id == "HEURISTIC_CHARGING_VOLTAGE" && item.active;
    }));
}
