#include <spdlog/spdlog.h>
#include "revdash/core/types.hpp"

#if defined(REVDASH_HAS_QT)
#include "app/app_controller.hpp"
#include "app/telemetry_chart_item.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#endif

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    spdlog::info("Launching {} Desktop v{}", revdash::core::kApplicationName, revdash::core::kApplicationVersion);

#if defined(REVDASH_HAS_QT)
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("RevDash"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("revdash.local"));
    app.setApplicationName(QString::fromUtf8(revdash::core::kApplicationName.data(), revdash::core::kApplicationName.size()));
    app.setApplicationVersion(QString::fromUtf8(revdash::core::kApplicationVersion.data(), revdash::core::kApplicationVersion.size()));

    revdash::app::AppController controller;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("RevDash"), QStringLiteral("Main"));
    return app.exec();
#else
    spdlog::info("Qt GUI framework not linked in current configuration.");
    return 0;
#endif
}
