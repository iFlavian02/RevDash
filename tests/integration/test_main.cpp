#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "revdash/core/bounded_spsc_queue.hpp"
#include "revdash/core/engine_service.hpp"
#include "revdash/core/latest_telemetry_store.hpp"
#include "revdash/core/metric_aggregator.hpp"
#include "revdash/core/pipeline_packets.hpp"
#include "revdash/diagnostics/rule_evaluator.hpp"
#include "revdash/drivers/playback.hpp"
#include "revdash/drivers/synthetic.hpp"
#include "revdash/protocol/mode01.hpp"
#include "revdash/session/session_recorder.hpp"

namespace {

std::atomic<bool> g_track_allocations{false};
std::atomic<std::uint64_t> g_allocation_count{0};
std::atomic<std::uint64_t> g_deallocation_count{0};

void countAllocation() noexcept {
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void countDeallocation() noexcept {
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_deallocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

void* operator new(std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
    countDeallocation();
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    countDeallocation();
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    countDeallocation();
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    countDeallocation();
    std::free(memory);
}

namespace {

using namespace std::chrono_literals;
using revdash::core::DiagnosticSnapshot;
using revdash::core::EngineService;
using revdash::core::MetricId;
using revdash::core::SyntheticConfig;
using Json = nlohmann::json;

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Scenario {
    std::string_view name;
    SyntheticConfig config;
    std::optional<std::string_view> expected_dtc;
};

constexpr std::array<MetricId, 5> kAcceptanceMetrics{
    MetricId::Rpm,
    MetricId::CoolantTemp,
    MetricId::LongTermFuelTrim1,
    MetricId::AmbientAirTemp,
    MetricId::ModuleVoltage,
};

constexpr std::array<std::uint8_t, 5> kAcceptancePids{0x0C, 0x05, 0x07, 0x46, 0x42};

bool hasValidAcceptanceTelemetry(const EngineService& engine) {
    const auto snapshot = engine.telemetrySnapshot();
    return std::ranges::all_of(kAcceptanceMetrics, [&snapshot](MetricId metric) {
        const auto& sample = snapshot.get(metric);
        return sample.isValid() && std::isfinite(sample.value);
    });
}

void installAndConnect(EngineService& engine, const SyntheticConfig& config) {
    std::atomic<bool> installed{false};
    std::atomic<bool> install_succeeded{false};
    engine.setSource(
        std::make_unique<revdash::drivers::SyntheticDataSource>(),
        [&](auto result) {
            install_succeeded.store(result.has_value(), std::memory_order_relaxed);
            installed.store(true, std::memory_order_release);
        });
    REQUIRE(waitFor([&] { return installed.load(std::memory_order_acquire); }));
    REQUIRE(install_succeeded.load(std::memory_order_relaxed));

    std::atomic<bool> connected{false};
    std::atomic<bool> connect_succeeded{false};
    engine.connect(config, [&](auto result) {
        connect_succeeded.store(result.has_value(), std::memory_order_relaxed);
        connected.store(true, std::memory_order_release);
    });
    REQUIRE(waitFor([&] { return connected.load(std::memory_order_acquire); }));
    REQUIRE(connect_succeeded.load(std::memory_order_relaxed));
}

DiagnosticSnapshot scan(EngineService& engine) {
    DiagnosticSnapshot snapshot;
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    engine.scan([&](auto result) {
        if (result) {
            snapshot = std::move(*result);
            succeeded.store(true, std::memory_order_relaxed);
        }
        completed.store(true, std::memory_order_release);
    });
    REQUIRE(waitFor([&] { return completed.load(std::memory_order_acquire); }));
    REQUIRE(succeeded.load(std::memory_order_relaxed));
    return snapshot;
}

void disconnectWithinTarget(EngineService& engine) {
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    const auto started = std::chrono::steady_clock::now();
    engine.disconnect([&](auto result) {
        succeeded.store(result.has_value(), std::memory_order_relaxed);
        completed.store(true, std::memory_order_release);
    });
    REQUIRE(waitFor([&] { return completed.load(std::memory_order_acquire); }, 2s));
    CHECK(succeeded.load(std::memory_order_relaxed));
    CHECK(std::chrono::steady_clock::now() - started <= 2s);
}

class TempDirectory {
public:
    TempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("revdash-backend-e2e-" + revdash::session::generateSessionUuid())) {
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string speedPayload(std::uint8_t speed) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string payload{"410D00"};
    payload[4] = digits[(speed >> 4U) & 0x0FU];
    payload[5] = digits[speed & 0x0FU];
    return payload;
}

std::filesystem::path playbackAcceptanceSession(const TempDirectory& directory, std::size_t packet_count) {
    const auto path = directory.path() / "playback-250pps.jsonl";
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    output << Json{{"schema_version", 1}, {"type", "header"}, {"elapsed_us", 0},
                   {"uuid", "12345678-1234-4234-8234-123456789ABC"},
                   {"application_version", "acceptance"}, {"utc_start", "2026-01-01T00:00:00.000Z"},
                   {"source_type", "Synthetic"}, {"adapter_metadata", Json::object()},
                   {"protocol_metadata", Json::object()}, {"vehicle_metadata", Json::array()},
                   {"simulation", nullptr}}
                  .dump()
           << '\n';
    for (std::size_t index = 0; index < packet_count; ++index) {
        const auto elapsed_us = static_cast<std::int64_t>(index) * 20'000;
        const auto speed = static_cast<std::uint8_t>(index % 121U);
        output << Json{{"schema_version", 1}, {"type", "obd_message"}, {"elapsed_us", elapsed_us},
                       {"source_type", "Synthetic"}, {"sequence", index + 1U},
                       {"ecu", {{"format", "can_11_bit"}, {"value", 0x7E8}}},
                       {"payload_hex", speedPayload(speed)}}
                      .dump()
               << '\n';
    }
    const auto final_elapsed = static_cast<std::int64_t>(packet_count - 1U) * 20'000;
    output << Json{{"schema_version", 1}, {"type", "footer"}, {"elapsed_us", final_elapsed},
                   {"statistics", {{"obd_messages", packet_count}, {"telemetry_samples", 0},
                                   {"dtcs", 0}, {"diagnostic_findings", 0}, {"mode04_audits", 0},
                                   {"ecu_metadata", 0}, {"data_loss_markers", 0}, {"dropped_records", 0},
                                   {"serialization_buffer_growths", 0}, {"total_records", packet_count + 2U}}}}
                  .dump()
           << '\n';
    output.close();
    REQUIRE(output);
    return path;
}

} // namespace

TEST_CASE("Synthetic backend scenarios cross the complete telemetry pipeline", "[integration][backend_e2e]") {
    const std::array scenarios{
        Scenario{"normal engine", SyntheticConfig{.deterministic_seed = 101}, std::nullopt},
        Scenario{"vacuum leak", SyntheticConfig{.deterministic_seed = 102, .inject_vacuum_leak = true}, "P0171"},
        Scenario{"misfire", SyntheticConfig{.deterministic_seed = 103, .inject_misfire = true}, "P0300"},
        Scenario{"thermostat", SyntheticConfig{.deterministic_seed = 104, .inject_thermostat_fault = true}, "P0128"},
        Scenario{"noisy/dropout stream",
                 SyntheticConfig{.deterministic_seed = 105, .noise_std_dev = 0.5, .packet_dropout_prob = 0.15},
                 std::nullopt},
    };

    for (const auto& scenario : scenarios) {
        DYNAMIC_SECTION(scenario.name) {
            EngineService engine;
            std::atomic<std::uint64_t> recorded_messages{0};
            engine.setRecorderHandler([&](const auto&) {
                recorded_messages.fetch_add(1, std::memory_order_relaxed);
            });

            installAndConnect(engine, scenario.config);
            engine.setSupportedPids({kAcceptancePids.begin(), kAcceptancePids.end()});

            REQUIRE(waitFor([&] { return hasValidAcceptanceTelemetry(engine); }));
            REQUIRE(waitFor([&] { return recorded_messages.load(std::memory_order_relaxed) >= kAcceptancePids.size(); }));

            const auto telemetry = engine.telemetrySnapshot();
            CHECK(telemetry.get(MetricId::Rpm).value >= 500.0);
            CHECK(telemetry.get(MetricId::ModuleVoltage).value >= 13.0);
            CHECK(telemetry.get(MetricId::ModuleVoltage).value <= 15.0);
            if (scenario.config.inject_vacuum_leak) {
                CHECK(telemetry.get(MetricId::LongTermFuelTrim1).value >= 10.0);
            } else if (scenario.config.noise_std_dev == 0.0) {
                CHECK(std::abs(telemetry.get(MetricId::LongTermFuelTrim1).value) <= 2.0);
            }

            const auto diagnostics = scan(engine);
            if (scenario.expected_dtc) {
                CHECK(std::ranges::any_of(diagnostics.dtcs, [&](const auto& dtc) {
                    return dtc.code == *scenario.expected_dtc;
                }));
            } else {
                CHECK(diagnostics.dtcs.empty());
                CHECK(diagnostics.findings.empty());
            }

            CHECK(engine.sourceQueueHealth().dropped == 0);
            CHECK(engine.recorderQueueHealth().dropped == 0);
            CHECK(engine.recorderQueueHealth().popped >= recorded_messages.load(std::memory_order_relaxed));
            disconnectWithinTarget(engine);
        }
    }
}

TEST_CASE("Synthetic backend shutdown remains bounded with active delayed I/O", "[integration][backend_e2e]") {
    EngineService engine;
    installAndConnect(engine, SyntheticConfig{.deterministic_seed = 201, .response_latency = 100ms});
    engine.setSupportedPids({0x0C, 0x0D, 0x11});
    REQUIRE(waitFor([&] { return engine.sourceQueueHealth().pushed > 0; }));
    disconnectWithinTarget(engine);
}

TEST_CASE("Steady telemetry pipeline profiles 100000 frames with bounded allocation", "[integration][backend_e2e]") {
    constexpr std::uint64_t frame_count = 100'000;
    auto source_queue = std::make_unique<revdash::core::SourceToEngineQueue>();
    auto recorder_queue = std::make_unique<revdash::core::EngineToRecorderQueue>();
    revdash::core::LatestTelemetryStore telemetry;
    revdash::core::MetricAggregator aggregator;
    revdash::diagnostics::DiagnosticRuleEvaluator rules;

    const std::array<std::uint8_t, 4> payload{0x41, 0x0C, 0x1F, 0x40}; // 2000 rpm
    auto message = revdash::core::ObdMessage::create(
        revdash::core::DataSourceType::Synthetic, revdash::core::EcuAddress{0x7E8}, payload, 1,
        revdash::core::MonotonicTimePoint{}, std::nullopt);
    REQUIRE(message);

    revdash::core::SourceToEnginePacket source_packet{.engine_epoch = 1, .message = *message};
    revdash::core::SourceToEnginePacket dequeued;
    revdash::core::RecorderPacket recorder_packet;
    REQUIRE(source_queue->tryPush(source_packet));
    REQUIRE(source_queue->tryPop(dequeued));
    REQUIRE(recorder_queue->tryPush({.engine_epoch = 1, .message = dequeued.message}));
    REQUIRE(recorder_queue->tryPop(recorder_packet));

    g_allocation_count.store(0, std::memory_order_relaxed);
    g_deallocation_count.store(0, std::memory_order_relaxed);
    g_track_allocations.store(true, std::memory_order_release);
    bool queue_ok = true;
    for (std::uint64_t index = 0; index < frame_count; ++index) {
        source_packet.message.sequence_number = index + 1U;
        queue_ok = source_queue->tryPush(source_packet) && source_queue->tryPop(dequeued) &&
                   recorder_queue->tryPush({.engine_epoch = 1, .message = dequeued.message}) &&
                   recorder_queue->tryPop(recorder_packet) && queue_ok;
    }
    g_track_allocations.store(false, std::memory_order_release);
    REQUIRE(queue_ok);
    REQUIRE(g_allocation_count.load(std::memory_order_relaxed) == 0);

    g_allocation_count.store(0, std::memory_order_relaxed);
    g_deallocation_count.store(0, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    g_track_allocations.store(true, std::memory_order_release);
    std::uint64_t decoded_samples = 0;
    const auto process_frames = [&](std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t index = first; index < first + count; ++index) {
            auto frame = *message;
            frame.sequence_number = index + 1U;
            frame.monotonic_ts = revdash::core::MonotonicTimePoint{} + std::chrono::milliseconds{index * 10U};
            const auto decoded = revdash::protocol::decodeMode01Response(frame, 0x0C);
            if (!decoded) continue;
            for (const auto& sample : *decoded) {
                telemetry.update(sample);
                aggregator.ingest(sample);
                rules.ingest(sample);
                ++decoded_samples;
            }
            static_cast<void>(rules.evaluate(frame.monotonic_ts));
        }
    };
    process_frames(0, frame_count);
    g_track_allocations.store(false, std::memory_order_release);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto allocations = g_allocation_count.load(std::memory_order_relaxed);
    const auto deallocations = g_deallocation_count.load(std::memory_order_relaxed);
    const auto retained_allocations = allocations - std::min(allocations, deallocations);
    const auto frames_per_second = static_cast<double>(frame_count) /
        std::chrono::duration<double>(elapsed).count();

    constexpr std::uint64_t sustained_frames = 10'000;
    g_allocation_count.store(0, std::memory_order_relaxed);
    g_deallocation_count.store(0, std::memory_order_relaxed);
    g_track_allocations.store(true, std::memory_order_release);
    process_frames(frame_count, sustained_frames);
    g_track_allocations.store(false, std::memory_order_release);
    const auto sustained_allocations = g_allocation_count.load(std::memory_order_relaxed);
    const auto sustained_deallocations = g_deallocation_count.load(std::memory_order_relaxed);
    const auto sustained_growth = sustained_allocations - std::min(sustained_allocations, sustained_deallocations);

    INFO("frames/s=" << frames_per_second << ", allocations=" << allocations
         << ", retained allocation upper bound=" << retained_allocations
         << ", post-window growth=" << sustained_growth);
    CHECK(decoded_samples == frame_count + sustained_frames);
    CHECK(telemetry.snapshot().get(MetricId::Rpm).value == 2000.0);
    CHECK(frames_per_second >= 250.0);
#if defined(REVDASH_ASAN_ENABLED)
    CHECK(allocations <= frame_count * 10U);
#else
    CHECK(allocations <= frame_count * 3U);
#endif
    CHECK(retained_allocations < 75'000U);
    CHECK(sustained_growth < 1'000U);
    CHECK(source_queue->health().dropped == 0);
    CHECK(recorder_queue->health().dropped == 0);
}

TEST_CASE("Five-times playback sustains equivalent 250 packet-per-second load", "[integration][backend_e2e]") {
    constexpr std::size_t packet_count = 500;
    TempDirectory directory;
    const auto session = playbackAcceptanceSession(directory, packet_count);
    EngineService engine;
    std::atomic<std::uint64_t> recorded_messages{0};
    engine.setRecorderHandler([&](const auto&) {
        recorded_messages.fetch_add(1, std::memory_order_relaxed);
    });

    std::atomic<bool> installed{false};
    std::atomic<bool> install_succeeded{false};
    engine.setSource(std::make_unique<revdash::drivers::PlaybackDataSource>(), [&](auto result) {
        install_succeeded.store(result.has_value(), std::memory_order_relaxed);
        installed.store(true, std::memory_order_release);
    });
    REQUIRE(waitFor([&] { return installed.load(std::memory_order_acquire); }));
    REQUIRE(install_succeeded.load(std::memory_order_relaxed));

    std::atomic<bool> connected{false};
    std::atomic<bool> connect_succeeded{false};
    engine.connect(revdash::core::PlaybackConfig{
        .session_file_path = session.string(), .speed_multiplier = 5.0, .loop = false},
        [&](auto result) {
            connect_succeeded.store(result.has_value(), std::memory_order_relaxed);
            connected.store(true, std::memory_order_release);
        });
    REQUIRE(waitFor([&] { return connected.load(std::memory_order_acquire); }));
    REQUIRE(connect_succeeded.load(std::memory_order_relaxed));

    std::atomic<bool> playing{false};
    std::atomic<bool> play_succeeded{false};
    const auto started = std::chrono::steady_clock::now();
    engine.startPlayback([&](auto result) {
        play_succeeded.store(result.has_value(), std::memory_order_relaxed);
        playing.store(true, std::memory_order_release);
    });
    REQUIRE(waitFor([&] { return playing.load(std::memory_order_acquire); }));
    REQUIRE(play_succeeded.load(std::memory_order_relaxed));
    REQUIRE(waitFor([&] { return recorded_messages.load(std::memory_order_relaxed) == packet_count; }, 5s));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(elapsed <= 3s);
    CHECK(engine.telemetrySnapshot().get(MetricId::VehicleSpeed).value ==
          static_cast<double>((packet_count - 1U) % 121U));
    CHECK(engine.sourceQueueHealth().pushed == packet_count);
    CHECK(engine.sourceQueueHealth().popped == packet_count);
    CHECK(engine.sourceQueueHealth().dropped == 0);
    CHECK(engine.recorderQueueHealth().pushed == packet_count);
    CHECK(engine.recorderQueueHealth().popped == packet_count);
    CHECK(engine.recorderQueueHealth().dropped == 0);
    disconnectWithinTarget(engine);
}
