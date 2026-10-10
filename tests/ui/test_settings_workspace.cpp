#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <qqml.h>


#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"
#include "revdash/diagnostics/dtc_database.hpp"

using namespace revdash;

TEST_CASE("settings persist units theme paths and connection preferences", "[settings_workspace]") {
    QTemporaryDir settings_root; QTemporaryDir data_root; REQUIRE(settings_root.isValid()); REQUIRE(data_root.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_root.path());
    QCoreApplication::setOrganizationName(QStringLiteral("RevDashTests")); QCoreApplication::setApplicationName(QStringLiteral("SettingsRoundTrip"));
    { QSettings settings; settings.clear(); }
    { app::AppController controller; controller.setDarkTheme(false); controller.setImperial(true); controller.setSessionPath(data_root.filePath(QStringLiteral("sessions"))); controller.setExportPath(data_root.filePath(QStringLiteral("exports"))); controller.setPreferredPort(QStringLiteral("COM9")); controller.setPreferredBaud(115200); }
    { app::AppController controller; REQUIRE_FALSE(controller.darkTheme()); REQUIRE(controller.imperial()); REQUIRE(controller.preferredPort()==QStringLiteral("COM9")); REQUIRE(controller.preferredBaud()==115200); REQUIRE(QFileInfo(controller.sessionPath()).isDir()); REQUIRE(QFileInfo(controller.exportPath()).isDir()); }
}

TEST_CASE("settings recover invalid stored paths and explain a missing production database", "[settings_workspace]") {
    QTemporaryDir directory; REQUIRE(directory.isValid()); const auto file=directory.filePath(QStringLiteral("not-a-directory")); QFile marker(file); REQUIRE(marker.open(QIODevice::WriteOnly)); marker.close();
    app::AppController controller; controller.setSessionPath(file); REQUIRE(QFileInfo(controller.sessionPath()).isDir());
    controller.setDtcDatabasePath(directory.filePath(QStringLiteral("missing.sqlite"))); controller.lookupDtc(QStringLiteral("P0300")); REQUIRE(controller.dtcLookupMessage().contains(QStringLiteral("unavailable"),Qt::CaseInsensitive));
}

TEST_CASE("settings and DTC lookup QML exposes persistent controls and results", "[settings_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash",1,0,"TelemetryChartItem"); app::AppController controller; QQmlEngine engine;
    QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml"))); INFO(component.errorString().toStdString()); REQUIRE(component.status()==QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"),QVariant::fromValue(&controller)}})); REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("settingsWorkspace"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("settingsImperialToggle"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("dtcLookupInput"))); REQUIRE(root->findChild<QObject*>(QStringLiteral("dtcLookupList")));
}
