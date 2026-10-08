#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>
#include <qqml.h>

#include <functional>

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
}

TEST_CASE("diagnostics model groups ECU records and exposes freeze-frame detail", "[diagnostics_workspace]") {
    app::DtcModel model;
    core::DtcRecord stored{.code = "P0300", .status = core::DtcStatus::Confirmed, .severity = core::Severity::Warning,
        .description = "Random misfire", .likely_failure_points = {"Spark plugs"}, .ecu_address = core::EcuAddress{0x7E8},
        .freeze_frame = core::FreezeFrame{.dtc_code = "P0300", .frame_number = 0, .timestamp = core::MonotonicClock::now(),
            .samples = {{.metric_id = core::MetricId::Rpm, .value = 812.0, .quality = core::SampleQuality::Valid}}}};
    core::DtcRecord pending{.code = "P0171", .status = core::DtcStatus::Pending, .severity = core::Severity::Advisory,
        .description = "Lean", .likely_failure_points = {}, .ecu_address = core::EcuAddress{0x7E9}, .freeze_frame = std::nullopt};
    model.setRecords({stored, pending});

    REQUIRE(model.data(model.index(0), app::DtcModel::GroupRole).toString() == QStringLiteral("Stored"));
    REQUIRE(model.data(model.index(1), app::DtcModel::GroupRole).toString() == QStringLiteral("Pending"));
    REQUIRE(model.data(model.index(0), app::DtcModel::EcuRole).toString() == QStringLiteral("0X7E8"));
    REQUIRE(model.data(model.index(0), app::DtcModel::HasFreezeFrameRole).toBool());
    REQUIRE(model.data(model.index(0), app::DtcModel::FreezeFrameSamplesRole).toList().size() == 1);
}

TEST_CASE("finding model presents status evidence and limitations", "[diagnostics_workspace]") {
    app::FindingModel model;
    core::DiagnosticFinding finding;
    finding.rule_id = "HEURISTIC_CATALYST";
    finding.title = "Catalyst behavior";
    finding.evidence = {"Downstream switching follows upstream"};
    model.setFindings({finding});
    REQUIRE(model.data(model.index(0), app::FindingModel::StatusRole).toString() == QStringLiteral("Active"));
    REQUIRE(model.data(model.index(0), app::FindingModel::EvidenceRole).toStringList().size() == 1);
    REQUIRE_FALSE(model.data(model.index(0), app::FindingModel::LimitationsRole).toString().isEmpty());
}

TEST_CASE("raw diagnostic terminal is bounded pausable filterable and copyable", "[diagnostics_workspace]") {
    app::RawDiagnosticModel model;
    std::vector<std::string> lines;
    for (int index = 0; index < 550; ++index) lines.push_back(index == 549 ? "0x7E8  43 03 00" : "0x7E9  47 01 71");
    model.setLines(lines);
    REQUIRE(model.rowCount() == app::RawDiagnosticModel::kMaximumLines);
    model.setHexFilter(QStringLiteral("43 03"));
    REQUIRE(model.rowCount() == 1);
    REQUIRE(model.copyText().contains(QStringLiteral("43 03 00")));
    model.setPaused(true);
    model.setLines({"0x7E8  44"});
    REQUIRE(model.rowCount() == 1);
}

TEST_CASE("diagnostics scan binds the synthetic multi-ECU snapshot", "[diagnostics_workspace]") {
    app::AppController controller;
    controller.connectSynthetic(QStringLiteral("Fault demo"), 77);
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
    REQUIRE_FALSE(controller.clearDtcEnabled());
    controller.scanDiagnostics();
    REQUIRE(waitFor([&] { return !controller.diagnosticBusy(); }));
    REQUIRE(controller.dtcModel()->rowCount() >= 1);
}

TEST_CASE("diagnostics QML exposes scan inspectors terminal and guarded clear", "[diagnostics_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    app::AppController controller;
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("diagnosticsWorkspace")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("dtcList")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("findingList")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("rawDiagnosticTerminal")));
    auto* clear_button = root->findChild<QObject*>(QStringLiteral("clearDtcButton"));
    REQUIRE(clear_button);
    REQUIRE_FALSE(clear_button->property("enabled").toBool());
}
