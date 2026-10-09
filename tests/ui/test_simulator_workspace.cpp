#include <catch2/catch_approx.hpp>
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

TEST_CASE("simulator workspace dispatches controls and preserves its seed", "[simulator_workspace]") {
    app::AppController controller;
    REQUIRE_FALSE(controller.simulatorControlsEnabled());
    controller.connectSynthetic(QStringLiteral("Balanced"), 8675309);
    REQUIRE(waitFor([&] { return controller.simulatorControlsEnabled(); }));
    REQUIRE(controller.configuredSeed() == 8675309);

    controller.setSimulationThrottle(47.0);
    controller.setSimulationAmbientTemperature(-12.0);
    controller.setSimulationFaults(true, true, true, 1.25, 0.15);
    REQUIRE(waitFor([&] {
        return controller.simulationThrottle() == Catch::Approx(47.0) &&
               controller.simulationAmbientTemperature() == Catch::Approx(-12.0) &&
               controller.simulationMisfire() && controller.simulationVacuumLeak() &&
               controller.simulationThermostatFault() &&
               controller.simulationNoise() == Catch::Approx(1.25) &&
               controller.simulationDropout() == Catch::Approx(0.15);
    }));
}

TEST_CASE("simulator workspace controls ignition and exposes true and observed state", "[simulator_workspace]") {
    app::AppController controller;
    controller.connectSynthetic(QStringLiteral("Balanced"), 42);
    REQUIRE(waitFor([&] { return controller.simulatorControlsEnabled() && controller.simulationObservedRpm() > 0.0; }));
    REQUIRE(controller.simulationTrueRpm() > 0.0);

    controller.setSimulationIgnition(false);
    REQUIRE(waitFor([&] { return !controller.simulationIgnitionOn() && !controller.simulationEngineRunning() && controller.simulationTrueRpm() == 0.0; }));
    controller.setSimulationIgnition(true);
    controller.setSimulationEngineRunning(true);
    REQUIRE(waitFor([&] { return controller.simulationEngineRunning() && controller.simulationTrueRpm() > 0.0; }));
}

TEST_CASE("simulator workspace locks controls for a physical source", "[simulator_workspace]") {
    app::AppController controller(std::make_unique<core::EngineService>(), [] {
        return std::vector<drivers::SerialPortInfo>{{.port_name="COM7", .friendly_name="Test adapter"}};
    });
    controller.connectSerial(QStringLiteral("COM7"), 38400);
    REQUIRE_FALSE(controller.simulatorControlsEnabled());
    controller.setSimulationThrottle(90.0);
    REQUIRE(controller.simulationThrottle() != Catch::Approx(90.0));
}

TEST_CASE("simulator workspace QML exposes controls state display and lockout", "[simulator_workspace]") {
    qmlRegisterType<app::TelemetryChartItem>("RevDash", 1, 0, "TelemetryChartItem");
    app::AppController controller;
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REVDASH_QML_SOURCE_DIR "/Main.qml")));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    REQUIRE(root);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulatorWorkspace")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulatorControlPanel")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulationStatePanel")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulationThrottleSlider")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulationMisfireToggle")));
    REQUIRE(root->findChild<QObject*>(QStringLiteral("simulatorLockout")));
}
