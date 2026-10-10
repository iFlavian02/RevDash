#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QThread>
#include <QFileInfo>
#include <qqml.h>

#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"

using namespace revdash;

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout_ms = 3000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(1); }
    return predicate();
}
void writeSession(const QString& path, bool partial) {
    std::ofstream output{std::filesystem::path{path.toStdWString()}};
    output << nlohmann::json{{"schema_version",1},{"type","header"},{"elapsed_us",0},{"uuid","12345678-1234-4234-8234-123456789ABC"},{"application_version","test"},{"utc_start","2026-01-02T03:04:05.000Z"},{"source_type","Synthetic"},{"adapter_metadata",nlohmann::json::object()},{"protocol_metadata",nlohmann::json::object()},{"vehicle_metadata",nlohmann::json::array({{{"vin","TESTVIN123"}}})},{"simulation",nullptr}}.dump() << '\n';
    output << nlohmann::json{{"schema_version",1},{"type","obd_message"},{"elapsed_us",0},{"source_type","Synthetic"},{"sequence",1},{"ecu",{{"format","can_11_bit"},{"value",0x7e8}}},{"payload_hex","410D0A"}}.dump() << '\n';
    output << nlohmann::json{{"schema_version",1},{"type","dtc"},{"elapsed_us",500000},{"code","P0300"},{"status","Confirmed"},{"severity","Warning"},{"description","Misfire"},{"likely_failure_points",nlohmann::json::array()},{"ecu",{{"format","can_11_bit"},{"value",0x7e8}}},{"freeze_frame",nullptr}}.dump() << '\n';
    if (!partial) output << nlohmann::json{{"schema_version",1},{"type","footer"},{"elapsed_us",1000000},{"statistics",{{"obd_messages",1},{"telemetry_samples",0},{"dtcs",1},{"diagnostic_findings",0},{"mode04_audits",0},{"ecu_metadata",0},{"data_loss_markers",0},{"dropped_records",0},{"serialization_buffer_growths",0},{"total_records",4}}}}.dump() << '\n';
}
}

TEST_CASE("sessions workspace discovers metadata and recoverable partial files", "[sessions_workspace]") {
    QTemporaryDir directory; REQUIRE(directory.isValid());
    writeSession(directory.filePath(QStringLiteral("complete.jsonl")), false);
    writeSession(directory.filePath(QStringLiteral("interrupted.partial")), true);
    app::AppController controller; controller.setSessionPath(directory.path());
    REQUIRE(controller.sessionModel()->rowCount() == 2);
    bool saw_partial=false, saw_complete=false;
    for(int row=0;row<2;++row){ const auto index=controller.sessionModel()->index(row); const auto recoverable=controller.sessionModel()->data(index,app::SessionModel::RecoverableRole).toBool(); saw_partial|=recoverable; saw_complete|=!recoverable; REQUIRE(controller.sessionModel()->data(index,app::SessionModel::DtcCountRole).toULongLong()==1); }
    REQUIRE(saw_partial); REQUIRE(saw_complete);
}

TEST_CASE("sessions workspace loads playback and exports CSV", "[sessions_workspace]") {
    QTemporaryDir directory; REQUIRE(directory.isValid()); const auto path=directory.filePath(QStringLiteral("drive.jsonl")); writeSession(path,false);
    app::AppController controller; controller.setSessionPath(directory.path());
    for(int row=0;row<controller.sessionModel()->rowCount();++row) if(!controller.sessionModel()->data(controller.sessionModel()->index(row),app::SessionModel::RecoverableRole).toBool()) controller.selectSession(row);
    controller.playSession(); REQUIRE(waitFor([&]{return controller.playbackState()==QStringLiteral("Playing")||controller.playbackState()==QStringLiteral("Stopped");}));
    const auto output=directory.filePath(QStringLiteral("drive.csv")); controller.exportSelectedSession(output); REQUIRE(QFileInfo::exists(output));
}

TEST_CASE("sessions workspace QML exposes discovery playback scrub and export", "[sessions_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash",1,0,"TelemetryChartItem"); app::AppController controller; QQmlEngine engine;
    QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml"))); INFO(component.errorString().toStdString()); REQUIRE(component.status()==QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"),QVariant::fromValue(&controller)}})); REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("sessionsWorkspace"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("sessionList"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("playbackScrubber"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("exportDialog")));
}
