#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "revdash/core/engine_service.hpp"
#include "revdash/core/async_data_source.hpp"
#include "revdash/diagnostics/dtc_database.hpp"
#include "revdash/drivers/synthetic.hpp"

namespace {

enum class ClearBehavior { Positive, Negative, Timeout };

struct DiagnosticSourceBehavior {
    std::atomic<int> speed_kph{0};
    std::atomic<bool> speed_response_enabled{true};
    std::atomic<ClearBehavior> clear_behavior{ClearBehavior::Positive};
    std::atomic<bool> cleared{false};
    bool second_ecu{true};
    bool partial_metadata{false};
    std::shared_ptr<revdash::core::IClock> clock{std::make_shared<revdash::core::SystemClockSource>()};
    mutable std::mutex mutex;
    std::vector<revdash::core::ObdRequest> requests;
};

class DiagnosticDataSource final : public revdash::core::AsyncDataSource {
public:
    DiagnosticDataSource(revdash::core::DataSourceType type, std::shared_ptr<DiagnosticSourceBehavior> behavior)
        : AsyncDataSource(type), behavior_(std::move(behavior)) {}

protected:
    void startConnect(const revdash::core::DataSourceConfig&, revdash::core::CompletionCallback completion) override {
        completion(revdash::core::makeSuccess());
    }

    void startDisconnect(revdash::core::CompletionCallback completion) override { completion(revdash::core::makeSuccess()); }

    void startTransmit(const revdash::core::ObdRequest& request, revdash::core::CompletionCallback completion) override {
        {
            std::lock_guard lock(behavior_->mutex);
            behavior_->requests.push_back(request);
        }
        const auto publish = [this](std::initializer_list<std::uint8_t> bytes, std::uint32_t ecu = 0x7E8) {
            const auto message = revdash::core::ObdMessage::create(
                type(), revdash::core::EcuAddress{ecu}, bytes, 0,
                behavior_->clock->monotonicNow(), behavior_->clock->utcNow());
            REQUIRE(message.has_value());
            publishMessage(*message);
        };

        if (request.mode == 0x01 && request.pid == 0x0D && behavior_->speed_response_enabled.load()) {
            publish({0x41, 0x0D, static_cast<std::uint8_t>(behavior_->speed_kph.load())});
        } else if (request.mode == 0x03) {
            if (behavior_->cleared.load()) {
                publish({0x43, 0x00, 0x00});
            } else {
                publish({0x43, 0x03, 0x00});
                if (behavior_->second_ecu) publish({0x43, 0x03, 0x00}, 0x7E9);
            }
        } else if (request.mode == 0x07) {
            if (behavior_->cleared.load()) publish({0x47, 0x00, 0x00});
            else publish({0x47, 0x01, 0x71});
        } else if (request.mode == 0x02 && request.pid == 0x0C) {
            publish({0x42, 0x0C, 0x0C, 0x80});
        } else if (request.mode == 0x09 && request.pid == 0x02) {
            publish({0x49, 0x02, 0x01, '1', 'H', 'G', 'C', 'R', '2', 'F', '8', '3', 'H', 'A', '0', '0', '0', '0', '0', '0'});
            if (behavior_->second_ecu) publish({0x49, 0x02, 0x01, '2', 'H', 'G', 'C', 'R', '2', 'F', '8', '3', 'H', 'A', '0', '0', '0', '0', '0', '1'}, 0x7E9);
        } else if (request.mode == 0x09 && request.pid == 0x04) {
            publish({0x49, 0x04, 0x01, 'C', 'A', 'L', 'I', 'D', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '1'});
            if (behavior_->second_ecu) publish({0x49, 0x04, 0x01, 'C', 'A', 'L', 'I', 'D', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '2'}, 0x7E9);
        } else if (request.mode == 0x09 && request.pid == 0x06) {
            if (behavior_->partial_metadata) publish({0x7F, 0x09, 0x11});
            else {
                publish({0x49, 0x06, 0x01, 0x12, 0x34, 0x56, 0x78});
                if (behavior_->second_ecu) publish({0x49, 0x06, 0x01, 0x87, 0x65, 0x43, 0x21}, 0x7E9);
            }
        } else if (request.mode == 0x04) {
            if (behavior_->clear_behavior.load() == ClearBehavior::Timeout) {
                completion(revdash::core::makeError(revdash::core::ErrorCode::TransportTimeout, "Mode 04 timed out"));
                return;
            }
            if (behavior_->clear_behavior.load() == ClearBehavior::Negative) publish({0x7F, 0x04, 0x22});
            else { behavior_->cleared = true; publish({0x44}); }
        }
        completion(revdash::core::makeSuccess());
    }

private:
    std::shared_ptr<DiagnosticSourceBehavior> behavior_;
};

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void connectPhysical(
    revdash::core::EngineService& engine,
    const std::shared_ptr<DiagnosticSourceBehavior>& behavior,
    revdash::core::DataSourceType type = revdash::core::DataSourceType::SerialElm327
) {
    std::atomic<bool> installed{false};
    std::atomic<bool> connected{false};
    engine.setSource(std::make_unique<DiagnosticDataSource>(type, behavior), [&](auto result) { REQUIRE(result); installed = true; });
    REQUIRE(waitFor([&] { return installed.load(); }));
    revdash::core::DataSourceConfig config = type == revdash::core::DataSourceType::Playback
        ? revdash::core::DataSourceConfig{revdash::core::PlaybackConfig{.session_file_path = "fixture.jsonl"}}
        : revdash::core::DataSourceConfig{revdash::core::SerialConfig{.port_name = "COM_TEST"}};
    engine.connect(config, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
}

void waitForSpeed(revdash::core::EngineService& engine) {
    engine.setSupportedPids({0x0D});
    REQUIRE(waitFor([&] { return engine.telemetrySnapshot().isValid(revdash::core::MetricId::VehicleSpeed); }));
}

} // namespace

TEST_CASE("EngineService connects a source and routes decoded telemetry into snapshots", "[engine_pipeline]") {
    revdash::core::EngineService engine;
    std::atomic<bool> source_ready{false};
    std::atomic<bool> connected{false};
    engine.setSource(std::make_unique<revdash::drivers::SyntheticDataSource>(), [&](auto result) { REQUIRE(result); source_ready = true; });
    REQUIRE(waitFor([&] { return source_ready.load(); }));
    engine.connect(revdash::core::SyntheticConfig{}, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    engine.setSupportedPids({0x0C});
    REQUIRE(waitFor([&] { return engine.telemetrySnapshot().isValid(revdash::core::MetricId::Rpm); }));
    REQUIRE(engine.connectionState() == revdash::core::ConnectionState::Ready);
    REQUIRE(engine.sourceQueueHealth().pushed > 0);
}

TEST_CASE("EngineService serializes commands from another thread and invalidates epochs on source replacement", "[engine_pipeline]") {
    revdash::core::EngineService engine;
    std::atomic<bool> installed{false};
    std::jthread caller([&] {
        engine.setSource(std::make_unique<revdash::drivers::SyntheticDataSource>(), [&](auto result) { REQUIRE(result); installed = true; });
    });
    caller.join();
    REQUIRE(waitFor([&] { return installed.load(); }));
    const auto first_epoch = engine.epoch();
    installed = false;
    engine.setSource(std::make_unique<revdash::drivers::SyntheticDataSource>(), [&](auto result) { REQUIRE(result); installed = true; });
    REQUIRE(waitFor([&] { return installed.load(); }));
    REQUIRE(engine.epoch() > first_epoch);
    REQUIRE(engine.telemetrySnapshot().epoch == engine.epoch());
}

TEST_CASE("EngineService rejects guarded clear preparation for a synthetic source", "[engine_pipeline]") {
    revdash::core::EngineService engine;
    std::atomic<bool> source_ready{false};
    std::atomic<bool> connected{false};
    engine.setSource(std::make_unique<revdash::drivers::SyntheticDataSource>(), [&](auto result) { REQUIRE(result); source_ready = true; });
    REQUIRE(waitFor([&] { return source_ready.load(); }));
    engine.connect(revdash::core::SyntheticConfig{}, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    std::atomic<bool> completed{false};
    engine.prepareClear([&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.SafetyRejected"); completed = true; });
    REQUIRE(waitFor([&] { return completed.load(); }));
}

TEST_CASE("diagnostic_service scans stored and pending DTCs across ECUs and captures freeze frame", "[diagnostic_service]") {
    revdash::core::EngineService engine;
    auto behavior = std::make_shared<DiagnosticSourceBehavior>();
    connectPhysical(engine, behavior);
    waitForSpeed(engine);

    std::atomic<bool> completed{false};
    revdash::core::DiagnosticSnapshot result;
    engine.scan([&](auto scan) {
        REQUIRE(scan.has_value());
        result = *scan;
        completed = true;
    });
    REQUIRE(waitFor([&] { return completed.load(); }));
    REQUIRE(result.dtcs.size() == 3);
    REQUIRE(result.dtcs[0].code == "P0300");
    REQUIRE(result.dtcs[0].ecu_address == revdash::core::EcuAddress{0x7E8});
    REQUIRE(result.dtcs[0].freeze_frame.has_value());
    REQUIRE(result.dtcs[1].code == "P0300");
    REQUIRE(result.dtcs[1].ecu_address == revdash::core::EcuAddress{0x7E9});
    REQUIRE(result.dtcs[2].code == "P0171");
    REQUIRE(engine.diagnosticSnapshot().dtcs == result.dtcs);
    std::size_t requests_after_scan = 0;
    {
        std::lock_guard lock(behavior->mutex);
        requests_after_scan = behavior->requests.size();
    }
    REQUIRE(waitFor([&] {
        std::lock_guard lock(behavior->mutex);
        return behavior->requests.size() > requests_after_scan && behavior->requests.back().mode == 0x01;
    }));
}

TEST_CASE("diagnostic_service enriches scanned DTCs through the configured local database", "[diagnostic_service]") {
    const auto directory = std::filesystem::temp_directory_path() /
        ("revdash-engine-dtc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto database_path = directory / "fixture.sqlite";
    const auto csv_path = std::filesystem::path{__FILE__}.parent_path().parent_path() / "fixtures" / "dtc" / "fixture.csv";
    REQUIRE(revdash::diagnostics::importDtcCsv({
        .input_csv = csv_path,
        .output_database = database_path,
        .database_kind = revdash::diagnostics::DtcDatabaseKind::TestFixture}));
    auto opened = revdash::diagnostics::DtcDatabase::openReadOnly(database_path, revdash::diagnostics::DtcDatabaseKind::TestFixture);
    REQUIRE(opened.has_value());

    {
        revdash::core::EngineService engine;
        engine.setDtcDatabase(std::make_shared<revdash::diagnostics::DtcDatabase>(std::move(*opened)));
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        behavior->second_ecu = false;
        connectPhysical(engine, behavior);
        std::atomic<bool> completed{false};
        revdash::core::DiagnosticSnapshot result;
        engine.scan([&](auto scan) { REQUIRE(scan); result = *scan; completed = true; });
        REQUIRE(waitFor([&] { return completed.load(); }));
        REQUIRE(result.dtcs[0].description == "Random/Multiple Cylinder Misfire Detected");
        REQUIRE(result.dtcs[0].severity == revdash::core::Severity::Critical);
        REQUIRE_FALSE(result.dtcs[0].likely_failure_points.empty());
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("diagnostic_service identification preserves per-ECU provenance and tolerates partial Mode 09 support", "[diagnostic_service]") {
    revdash::core::EngineService engine;
    auto behavior = std::make_shared<DiagnosticSourceBehavior>();
    behavior->partial_metadata = true;
    connectPhysical(engine, behavior);
    std::atomic<bool> completed{false};
    std::vector<revdash::core::EcuMetadata> metadata;
    std::optional<revdash::core::Error> identification_error;
    engine.identify([&](auto identified) {
        if (identified) metadata = *identified;
        else identification_error = identified.error();
        completed = true;
    });
    REQUIRE(waitFor([&] { return completed.load(); }));
    const auto identification_message = identification_error ? identification_error->message : std::string{"no identification error"};
    INFO(identification_message);
    REQUIRE_FALSE(identification_error.has_value());
    REQUIRE(metadata.size() == 2);
    REQUIRE(metadata[0].ecu_address == revdash::core::EcuAddress{0x7E8});
    REQUIRE(metadata[0].vin == "1HGCR2F83HA000000");
    REQUIRE(metadata[0].calibration_ids.size() == 1);
    REQUIRE(metadata[0].cvns.empty());
    REQUIRE(metadata[1].ecu_address == revdash::core::EcuAddress{0x7E9});
    REQUIRE(engine.ecuMetadata().size() == 2);
}

TEST_CASE("diagnostic_service identifies deterministic synthetic ECUs through the normal engine path", "[diagnostic_service]") {
    revdash::core::EngineService engine;
    std::atomic<bool> installed{false};
    std::atomic<bool> connected{false};
    engine.setSource(std::make_unique<revdash::drivers::SyntheticDataSource>(), [&](auto result) { REQUIRE(result); installed = true; });
    REQUIRE(waitFor([&] { return installed.load(); }));
    engine.connect(revdash::core::SyntheticConfig{.include_second_ecu = true}, [&](auto result) { REQUIRE(result); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    std::atomic<bool> identified{false};
    std::vector<revdash::core::EcuMetadata> metadata;
    engine.identify([&](auto result) { REQUIRE(result); metadata = *result; identified = true; });
    REQUIRE(waitFor([&] { return identified.load(); }));
    REQUIRE(metadata.size() == 2);
    REQUIRE(metadata[0].vin.size() == 17);
    REQUIRE(metadata[0].calibration_ids.size() == 1);
    REQUIRE(metadata[0].cvns.size() == 1);
}

TEST_CASE("diagnostic_service Mode 04 preparation enforces physical stationary fresh speed", "[diagnostic_service]") {
    SECTION("valid stationary physical source") {
        revdash::core::EngineService engine;
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        std::atomic<bool> completed{false};
        engine.prepareClear([&](auto prepared) {
            REQUIRE(prepared);
            REQUIRE_FALSE(prepared->confirmation_token.empty());
            REQUIRE(prepared->warning == revdash::core::kClearDiagnosticWarning);
            REQUIRE(prepared->expires_at - prepared->snapshot.captured_at <= std::chrono::seconds{31});
            completed = true;
        });
        REQUIRE(waitFor([&] { return completed.load(); }));
    }
    SECTION("moving vehicle") {
        revdash::core::EngineService engine;
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        behavior->speed_kph = 1;
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        std::atomic<bool> completed{false};
        engine.prepareClear([&](auto prepared) { REQUIRE_FALSE(prepared); REQUIRE(prepared.error().code == "Diagnostics.SafetyRejected"); completed = true; });
        REQUIRE(waitFor([&] { return completed.load(); }));
    }
    SECTION("unsupported speed") {
        revdash::core::EngineService engine;
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        connectPhysical(engine, behavior);
        std::atomic<bool> completed{false};
        engine.prepareClear([&](auto prepared) { REQUIRE_FALSE(prepared); REQUIRE(prepared.error().code == "Diagnostics.SafetyRejected"); completed = true; });
        REQUIRE(waitFor([&] { return completed.load(); }));
    }
    SECTION("stale speed") {
        auto clock = std::make_shared<revdash::core::ManualClock>();
        revdash::core::EngineService engine(clock, std::chrono::milliseconds{0});
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        behavior->clock = clock;
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        behavior->speed_response_enabled = false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        clock->advance(std::chrono::seconds{2});
        std::atomic<bool> completed{false};
        engine.prepareClear([&](auto prepared) { REQUIRE_FALSE(prepared); REQUIRE(prepared.error().code == "Diagnostics.SafetyRejected"); completed = true; });
        REQUIRE(waitFor([&] { return completed.load(); }));
    }
    SECTION("playback source") {
        revdash::core::EngineService engine;
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        connectPhysical(engine, behavior, revdash::core::DataSourceType::Playback);
        std::atomic<bool> completed{false};
        engine.prepareClear([&](auto prepared) { REQUIRE_FALSE(prepared); REQUIRE(prepared.error().code == "Diagnostics.SafetyRejected"); completed = true; });
        REQUIRE(waitFor([&] { return completed.load(); }));
    }
}

TEST_CASE("diagnostic_service Mode 04 tokens expire are single-use and bind to engine epoch", "[diagnostic_service]") {
    SECTION("expiration") {
        auto clock = std::make_shared<revdash::core::ManualClock>();
        revdash::core::EngineService engine(clock, std::chrono::milliseconds{0});
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        behavior->clock = clock;
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        std::atomic<bool> prepared{false};
        std::string token;
        engine.prepareClear([&](auto result) { REQUIRE(result); token = result->confirmation_token; prepared = true; });
        REQUIRE(waitFor([&] { return prepared.load(); }));
        clock->advance(std::chrono::seconds{31});
        std::atomic<bool> confirmed{false};
        engine.confirmClear(token, [&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.TokenExpired"); confirmed = true; });
        REQUIRE(waitFor([&] { return confirmed.load(); }));
    }
    SECTION("source switch invalidates token") {
        revdash::core::EngineService engine(nullptr, std::chrono::milliseconds{0});
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        std::atomic<bool> prepared{false};
        std::string token;
        engine.prepareClear([&](auto result) { REQUIRE(result); token = result->confirmation_token; prepared = true; });
        REQUIRE(waitFor([&] { return prepared.load(); }));
        std::atomic<bool> switched{false};
        engine.setSource(std::make_unique<DiagnosticDataSource>(revdash::core::DataSourceType::SerialElm327, behavior), [&](auto result) { REQUIRE(result); switched = true; });
        REQUIRE(waitFor([&] { return switched.load(); }));
        std::atomic<bool> confirmed{false};
        engine.confirmClear(token, [&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.TokenInvalid"); confirmed = true; });
        REQUIRE(waitFor([&] { return confirmed.load(); }));
    }
}

TEST_CASE("diagnostic_service Mode 04 validates response rescans and writes a complete audit", "[diagnostic_service]") {
    revdash::core::EngineService engine(nullptr, std::chrono::milliseconds{0});
    auto behavior = std::make_shared<DiagnosticSourceBehavior>();
    connectPhysical(engine, behavior);
    waitForSpeed(engine);
    std::atomic<bool> prepared{false};
    std::string token;
    engine.prepareClear([&](auto result) { REQUIRE(result); token = result->confirmation_token; prepared = true; });
    REQUIRE(waitFor([&] { return prepared.load(); }));

    std::atomic<bool> mismatch_rejected{false};
    engine.confirmClear("WRONG-TOKEN", [&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.TokenInvalid"); mismatch_rejected = true; });
    REQUIRE(waitFor([&] { return mismatch_rejected.load(); }));

    std::atomic<bool> confirmed{false};
    revdash::core::Mode04AuditRecord audit;
    engine.confirmClear(token, [&](auto result) { REQUIRE(result); audit = *result; confirmed = true; });
    const auto clear_completed = waitFor([&] { return confirmed.load(); });
    if (!clear_completed) {
        std::string modes;
        {
            std::lock_guard lock(behavior->mutex);
            for (const auto& request : behavior->requests) {
                if (!modes.empty()) modes += ',';
                modes += std::to_string(request.mode);
            }
        }
        INFO("request modes: " << modes);
        INFO("audit count: " << engine.mode04AuditRecords().size());
        INFO("snapshot DTC count: " << engine.diagnosticSnapshot().dtcs.size());
    }
    REQUIRE(clear_completed);
    REQUIRE(audit.request_transmitted);
    REQUIRE(audit.positive_response);
    REQUIRE(audit.post_clear_rescan_completed);
    REQUIRE(audit.post_clear_dtcs.empty());
    REQUIRE(audit.warning == revdash::core::kClearDiagnosticWarning);
    REQUIRE_FALSE(audit.error.has_value());
    REQUIRE(engine.mode04AuditRecords().size() == 1);

    std::atomic<bool> reused{false};
    engine.confirmClear(token, [&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.TokenInvalid"); reused = true; });
    REQUIRE(waitFor([&] { return reused.load(); }));
}

TEST_CASE("diagnostic_service Mode 04 records negative responses and transport timeouts", "[diagnostic_service]") {
    for (const auto behavior_kind : {ClearBehavior::Negative, ClearBehavior::Timeout}) {
        revdash::core::EngineService engine(nullptr, std::chrono::milliseconds{0});
        auto behavior = std::make_shared<DiagnosticSourceBehavior>();
        behavior->clear_behavior = behavior_kind;
        connectPhysical(engine, behavior);
        waitForSpeed(engine);
        std::atomic<bool> prepared{false};
        std::string token;
        engine.prepareClear([&](auto result) { REQUIRE(result); token = result->confirmation_token; prepared = true; });
        REQUIRE(waitFor([&] { return prepared.load(); }));
        std::atomic<bool> confirmed{false};
        engine.confirmClear(token, [&](auto result) {
            REQUIRE_FALSE(result);
            REQUIRE(result.error().code == (behavior_kind == ClearBehavior::Negative ? "Protocol.NegativeResponse" : "Transport.Timeout"));
            confirmed = true;
        });
        REQUIRE(waitFor([&] { return confirmed.load(); }));
        const auto audits = engine.mode04AuditRecords();
        REQUIRE(audits.size() == 1);
        REQUIRE(audits[0].error.has_value());
        REQUIRE_FALSE(audits[0].post_clear_rescan_completed);
    }
}
