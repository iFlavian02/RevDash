#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>
#include <qqml.h>

#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"

using namespace revdash;

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout_ms = 2500) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}
}

TEST_CASE("connect workspace enumerates and refreshes serial sources", "[connect_workspace]") {
    int refresh_count = 0;
    auto enumerate = [&refresh_count] {
        ++refresh_count;
        std::vector<drivers::SerialPortInfo> ports{{.port_name="COM7", .friendly_name="USB ELM327", .device_description="USB serial"}};
        if (refresh_count > 1) ports.push_back({.port_name="COM12", .friendly_name="Bluetooth OBD", .device_description="Bluetooth SPP", .is_bluetooth_spp=true});
        return ports;
    };
    app::AppController controller(std::make_unique<core::EngineService>(), enumerate);
    REQUIRE(controller.sourceModel()->rowCount() >= 2);
    REQUIRE(controller.serialPortModel()->rowCount() == 1);
    controller.refreshSerialPorts();
    REQUIRE(controller.serialPortModel()->rowCount() == 2);
    REQUIRE(controller.serialPortModel()->data(controller.serialPortModel()->index(1), app::SerialPortModel::TransportRole).toString() == QStringLiteral("Bluetooth Classic"));
}

TEST_CASE("connect workspace connects disconnects and reconnects synthetic source", "[connect_workspace]") {
    app::AppController controller;
    controller.connectSynthetic(QStringLiteral("City traffic"), 77);
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
    REQUIRE(controller.configuredPreset() == QStringLiteral("City traffic"));
    REQUIRE(controller.configuredSeed() == 77);
    REQUIRE(waitFor([&] { return controller.adapterIdentity() == QStringLiteral("RevDash Synthetic"); }));

    controller.disconnectSource();
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Disconnected"); }));
    controller.connectSynthetic(QStringLiteral("Highway cruise"), 88);
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
    REQUIRE(controller.configuredSeed() == 88);
}

TEST_CASE("connect workspace presents failed configuration and guards source switches", "[connect_workspace]") {
    app::AppController controller;
    controller.connectSerial(QString{}, 123);
    REQUIRE_FALSE(controller.lastError().isEmpty());
    REQUIRE(controller.connectionState() == QStringLiteral("Disconnected"));

    controller.setClearConfirmationPending(true);
    REQUIRE_FALSE(controller.sourceOperationsEnabled());
    controller.connectSynthetic(QStringLiteral("Balanced"), 999);
    REQUIRE(controller.configuredSeed() == 12345);
    controller.setClearConfirmationPending(false);
    REQUIRE(controller.sourceOperationsEnabled());
}

TEST_CASE("presentation units do not alter source configuration", "[connect_workspace]") {
    app::AppController controller;
    controller.connectSynthetic(QStringLiteral("Fault demo"), 4242);
    REQUIRE(waitFor([&] { return controller.connectionState() == QStringLiteral("Ready"); }));
    controller.setImperial(true);
    REQUIRE(controller.configuredPreset() == QStringLiteral("Fault demo"));
    REQUIRE(controller.configuredSeed() == 4242);
}

TEST_CASE("connect workspace QML exposes guarded controls and status", "[connect_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    app::AppController controller(std::make_unique<core::EngineService>(), [] {
        return std::vector<drivers::SerialPortInfo>{{.port_name="COM3", .friendly_name="Test adapter"}};
    });
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("connectWorkspace")));
    auto* source_tabs = root->findChild<QObject*>(QStringLiteral("sourceTabs"));
    REQUIRE(source_tabs);
    REQUIRE(source_tabs->property("enabled").toBool());
    controller.setClearConfirmationPending(true);
    QCoreApplication::processEvents();
    REQUIRE_FALSE(source_tabs->property("enabled").toBool());
}
