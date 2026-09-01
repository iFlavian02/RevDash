#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "revdash/core/engine_service.hpp"
#include "revdash/drivers/synthetic.hpp"

namespace {

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
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

TEST_CASE("EngineService defers guarded clear until its dedicated diagnostics stage", "[engine_pipeline]") {
    revdash::core::EngineService engine;
    std::atomic<bool> completed{false};
    engine.prepareClear([&](auto result) { REQUIRE_FALSE(result); REQUIRE(result.error().code == "Diagnostics.Unsupported"); completed = true; });
    REQUIRE(waitFor([&] { return completed.load(); }));
}
