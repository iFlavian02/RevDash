#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>
#include <qqml.h>

#include <chrono>
#include <functional>
#include <memory>

#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"

using namespace revdash;

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout_ms = 3000) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}

QModelIndex metricIndex(const app::TelemetryModel& model, core::MetricId metric) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto index = model.index(row);
        if (model.data(index, app::TelemetryModel::MetricIdRole).toInt() == static_cast<int>(metric)) return index;
    }
    return {};
}
}

TEST_CASE("dashboard snapshot distinguishes valid stale and unsupported values", "[dashboard_workspace]") {
    app::TelemetryModel model;
    core::TelemetrySnapshot snapshot;

    auto& speed = snapshot.samples[static_cast<std::size_t>(core::MetricId::VehicleSpeed)];
    speed.metric_id = core::MetricId::VehicleSpeed;
    speed.value = 0.0;
    speed.quality = core::SampleQuality::Valid;
    speed.monotonic_ts = core::MonotonicClock::now() - std::chrono::milliseconds{25};

    auto& coolant = snapshot.samples[static_cast<std::size_t>(core::MetricId::CoolantTemp)];
    coolant.metric_id = core::MetricId::CoolantTemp;
    coolant.value = 0.0;
    coolant.quality = core::SampleQuality::Stale;
    coolant.monotonic_ts = core::MonotonicClock::now() - std::chrono::seconds{2};

    model.setSnapshot(snapshot);
    const auto speed_index = metricIndex(model, core::MetricId::VehicleSpeed);
    const auto coolant_index = metricIndex(model, core::MetricId::CoolantTemp);
    const auto map_index = metricIndex(model, core::MetricId::Map);
    REQUIRE(speed_index.isValid());
    REQUIRE(model.data(speed_index, app::TelemetryModel::ValidRole).toBool());
    REQUIRE(model.data(speed_index, app::TelemetryModel::ValueRole).toDouble() == 0.0);
    REQUIRE(model.data(speed_index, app::TelemetryModel::StateLabelRole).toString() == QStringLiteral("Live"));
    REQUIRE_FALSE(model.data(coolant_index, app::TelemetryModel::ValidRole).toBool());
    REQUIRE(model.data(coolant_index, app::TelemetryModel::StateLabelRole).toString() == QStringLiteral("Stale"));
    REQUIRE(model.data(coolant_index, app::TelemetryModel::SampleAgeMsRole).toLongLong() >= 1900);
    REQUIRE(model.data(map_index, app::TelemetryModel::StateLabelRole).toString() == QStringLiteral("Unsupported"));
}

TEST_CASE("dashboard applies presentation conversions without changing its snapshot", "[dashboard_workspace]") {
    app::TelemetryModel model;
    core::TelemetrySnapshot snapshot;
    auto& speed = snapshot.samples[static_cast<std::size_t>(core::MetricId::VehicleSpeed)];
    speed = {.metric_id = core::MetricId::VehicleSpeed, .value = 100.0, .quality = core::SampleQuality::Valid};
    auto& coolant = snapshot.samples[static_cast<std::size_t>(core::MetricId::CoolantTemp)];
    coolant = {.metric_id = core::MetricId::CoolantTemp, .value = 100.0, .quality = core::SampleQuality::Valid};
    auto& map = snapshot.samples[static_cast<std::size_t>(core::MetricId::Map)];
    map = {.metric_id = core::MetricId::Map, .value = 100.0, .quality = core::SampleQuality::Valid};
    model.setSnapshot(snapshot);
    model.setUnitSystem(app::TelemetryModel::UnitSystem::Imperial);

    REQUIRE(model.data(metricIndex(model, core::MetricId::VehicleSpeed), app::TelemetryModel::ValueRole).toDouble() == Catch::Approx(62.1371192));
    REQUIRE(model.data(metricIndex(model, core::MetricId::CoolantTemp), app::TelemetryModel::ValueRole).toDouble() == Catch::Approx(212.0));
    REQUIRE(model.data(metricIndex(model, core::MetricId::Map), app::TelemetryModel::ValueRole).toDouble() == Catch::Approx(14.5037738));
    REQUIRE(snapshot.get(core::MetricId::VehicleSpeed).value == 100.0);
}

TEST_CASE("dashboard chart ranges retain bounded ten hertz history", "[dashboard_workspace]") {
    app::TelemetryChartItem chart;
    chart.setMetricId(static_cast<int>(core::MetricId::Rpm));
    chart.setHistorySeconds(10);
    REQUIRE(chart.historyCapacity() == 100);
    for (int sample = 0; sample < 125; ++sample) chart.appendSample(static_cast<int>(core::MetricId::Rpm), sample);
    REQUIRE(chart.sampleCount() == 100);
    chart.setHistorySeconds(30);
    REQUIRE(chart.historyCapacity() == 300);
    chart.setHistorySeconds(120);
    REQUIRE(chart.historyCapacity() == 1200);
    for (int sample = 0; sample < 1400; ++sample) chart.appendSample(static_cast<int>(core::MetricId::Rpm), sample);
    REQUIRE(chart.sampleCount() == 1200);
}

TEST_CASE("dashboard reports live poll rate and queue health", "[dashboard_workspace]") {
    app::AppController controller;
    controller.connectSynthetic(QStringLiteral("Balanced"), 12345);
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
    REQUIRE(waitFor([&] { return controller.actualPollRate() > 0.0; }));
    REQUIRE(controller.sourceQueueDrops() == 0);
    REQUIRE(controller.recorderQueueDrops() == 0);
}

TEST_CASE("dashboard QML exposes health primary metrics and range controls", "[dashboard_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    app::AppController controller;
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("dashboardWorkspace")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("telemetryHealthPanel")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("primaryTelemetryGrid")));
    auto* range = root->findChild<QObject*>(QStringLiteral("chartRangeSelector"));
    auto* chart = root->findChild<app::TelemetryChartItem*>(QStringLiteral("telemetryChart"));
    REQUIRE(range);
    REQUIRE(chart);
    range->setProperty("currentIndex", 2);
    QCoreApplication::processEvents();
    REQUIRE(chart->historySeconds() == 120);
    REQUIRE(chart->historyCapacity() == 1200);
}
