#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <qqml.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"
#include "revdash/session/session_recorder.hpp"

using namespace std::chrono_literals;
using namespace revdash;

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout_ms = 5000) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}
}

TEST_CASE("full-rate recording remains responsive while navigating every workspace", "[ui_e2e]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    session::JsonlSessionRecorder recorder;
    session::SessionHeader header;
    header.uuid = "12345678-1234-4234-8234-123456789ABC";
    header.application_version = "ui-e2e";
    header.source_type = core::DataSourceType::Synthetic;
    const auto recording_path = std::filesystem::path{directory.filePath(QStringLiteral("ui-e2e.jsonl")).toStdWString()};
    REQUIRE(recorder.start(recording_path, header, core::MonotonicClock::now()));

    auto engine = std::make_unique<core::EngineService>();
    auto* engine_view = engine.get();
    std::atomic<std::uint64_t> recorded_packets{0};
    engine->setRecorderHandler([&](const core::RecorderPacket& packet) {
        if (recorder.record(packet.message)) recorded_packets.fetch_add(1, std::memory_order_relaxed);
    });

    {
        app::AppController controller(std::move(engine), [] { return std::vector<drivers::SerialPortInfo>{}; });
        QQmlEngine qml_engine;
        QQmlComponent component(&qml_engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
        INFO(component.errorString().toStdString());
        REQUIRE(component.status() == QQmlComponent::Ready);
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
        REQUIRE(root);
        auto* tabs = root->findChild<QObject*>(QStringLiteral("workspaceTabs"));
        REQUIRE(tabs);

        controller.connectSynthetic(QStringLiteral("Highway cruise"), 8701);
        REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
        REQUIRE(waitFor([&] { return controller.actualPollRate() > 20.0 && recorded_packets.load() >= 25; }));
        controller.resetUiPerformanceMetrics();

        int consecutive_long_stalls = 0;
        int maximum_consecutive_long_stalls = 0;
        QElapsedTimer heartbeat;
        heartbeat.start();
        QTimer heartbeat_timer;
        heartbeat_timer.setInterval(16);
        heartbeat_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&heartbeat_timer, &QTimer::timeout, [&] {
            const auto gap = heartbeat.restart();
            consecutive_long_stalls = gap > 250 ? consecutive_long_stalls + 1 : 0;
            maximum_consecutive_long_stalls = std::max(maximum_consecutive_long_stalls, consecutive_long_stalls);
        });
        heartbeat_timer.start();

        for (int pass = 0; pass < 20; ++pass) {
            for (int workspace = 0; workspace < 6; ++workspace) {
                REQUIRE(tabs->setProperty("currentIndex", workspace));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                REQUIRE(tabs->property("currentIndex").toInt() == workspace);
            }
        }
        REQUIRE(waitFor([&] { return controller.uiPerformanceSnapshot().engineSnapshotAge.sampleCount >= 30; }));
        heartbeat_timer.stop();

        const auto performance = controller.uiPerformanceSnapshot();
        INFO("snapshot age p95=" << performance.engineSnapshotAge.p95Ms
             << " ms, controller p95=" << performance.controllerProcessing.p95Ms
             << " ms, chart p95=" << performance.chartUpdateWork.p95Ms
             << " ms, update p95/p99=" << performance.mainThreadUpdateWork.p95Ms << "/"
             << performance.mainThreadUpdateWork.p99Ms << " ms, navigation p95="
             << performance.visibleUiLatency.p95Ms << " ms");
        CHECK(performance.engineSnapshotAge.p95Ms < 100.0);
        CHECK(performance.mainThreadUpdateWork.p95Ms < 8.0);
        CHECK(performance.mainThreadUpdateWork.p99Ms < 16.7);
        CHECK(performance.visibleUiLatency.p95Ms < 100.0);
        CHECK(maximum_consecutive_long_stalls < 2);
        CHECK(engine_view->sourceQueueHealth().dropped == 0);
        CHECK(engine_view->recorderQueueHealth().dropped == 0);

        // Disconnect during navigation, then reconnect under the same controller.
        REQUIRE(tabs->setProperty("currentIndex", 4));
        controller.disconnectSource();
        REQUIRE(tabs->setProperty("currentIndex", 0));
        REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Disconnected"); }));
        controller.connectSynthetic(QStringLiteral("Highway cruise"), 8702);
        REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
        controller.disconnectSource();
        REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Disconnected"); }));
    }

    REQUIRE(recorder.finish(core::MonotonicClock::now()));
    CHECK(recorded_packets.load(std::memory_order_relaxed) > 25);
    CHECK(std::filesystem::exists(recording_path));
}

TEST_CASE("latency percentiles ignore one isolated scheduler outlier", "[ui_e2e]") {
    app::AppController controller;
    controller.resetUiPerformanceMetrics();
    for (int index = 0; index < 100; ++index) controller.recordVisibleUiLatency(index == 0 ? 500.0 : 2.0);
    const auto latency = controller.uiPerformanceSnapshot().visibleUiLatency;
    CHECK(latency.p95Ms == 2.0);
    CHECK(latency.p99Ms == 2.0);
    CHECK(latency.maximumMs == 500.0);
}
