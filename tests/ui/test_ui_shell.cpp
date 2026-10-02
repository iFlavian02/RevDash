#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QThread>
#include <QElapsedTimer>
#include <qqml.h>

#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"
#include "revdash/drivers/synthetic.hpp"

using namespace revdash;

TEST_CASE("UI models update and convert units on their owner thread", "[ui_shell]") {
    app::TelemetryModel model;
    core::TelemetrySnapshot snapshot;
    auto& speed = snapshot.samples[static_cast<std::size_t>(core::MetricId::VehicleSpeed)];
    speed.metric_id = core::MetricId::VehicleSpeed;
    speed.value = 100.0;
    speed.quality = core::SampleQuality::Valid;
    model.setSnapshot(snapshot);
    const auto index = model.index(static_cast<int>(core::MetricId::VehicleSpeed));
    REQUIRE(model.data(index, app::TelemetryModel::ValueRole).toDouble() == 100.0);
    model.setUnitSystem(app::TelemetryModel::UnitSystem::Imperial);
    REQUIRE(model.data(index, app::TelemetryModel::ValueRole).toDouble() == Catch::Approx(62.1371192));
    REQUIRE(model.data(index, app::TelemetryModel::UnitRole).toString() == QStringLiteral("mph"));
    REQUIRE(model.thread() == QThread::currentThread());
}

TEST_CASE("diagnostic models reset deterministically", "[ui_shell]") {
    app::DtcModel dtcs;
    core::DtcRecord record;
    record.code = "P0300";
    record.description = "Random misfire";
    dtcs.setRecords({record});
    REQUIRE(dtcs.rowCount() == 1);
    REQUIRE(dtcs.data(dtcs.index(0), app::DtcModel::CodeRole).toString() == QStringLiteral("P0300"));

    app::FindingModel findings;
    core::DiagnosticFinding finding;
    finding.rule_id = "charging";
    finding.title = "Charging voltage";
    findings.setFindings({finding});
    REQUIRE(findings.rowCount() == 1);
    findings.setFindings({});
    REQUIRE(findings.rowCount() == 0);
}

TEST_CASE("chart history is bounded and accepts deterministic samples", "[ui_shell]") {
    app::TelemetryChartItem chart;
    chart.setMetricId(1);
    chart.setHistoryCapacity(3);
    chart.appendSample(0, 99.0);
    chart.appendSample(1, 1.0);
    chart.appendSample(1, 2.0);
    chart.appendSample(1, 3.0);
    chart.appendSample(1, 4.0);
    REQUIRE(chart.sampleCount() == 3);
}

TEST_CASE("controller marshals worker events to the GUI thread", "[ui_shell]") {
    auto engine = std::make_unique<core::EngineService>();
    auto* engineRaw = engine.get();
    app::AppController controller(std::move(engine));
    bool ready = false;
    bool wrongThread = false;
    QObject::connect(&controller, &app::AppController::connectionStateChanged, &controller, [&] {
        wrongThread = wrongThread || QThread::currentThread() != controller.thread();
        ready = ready || controller.connectionState() == QStringLiteral("Ready");
    });
    engineRaw->setSource(std::make_unique<drivers::SyntheticDataSource>(), [engineRaw](core::Result<void> result) {
        if (result) engineRaw->connect(core::SyntheticConfig{});
    });
    QElapsedTimer timeout;
    timeout.start();
    while (!ready && timeout.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    REQUIRE(ready);
    REQUIRE_FALSE(wrongThread);
}

TEST_CASE("controller switches theme state", "[ui_shell]") {
    app::AppController controller;
    REQUIRE(controller.darkTheme());
    controller.setDarkTheme(false);
    REQUIRE_FALSE(controller.darkTheme());
}

TEST_CASE("application QML component loads", "[ui_shell]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    app::AppController controller;
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    REQUIRE(root != nullptr);
}
