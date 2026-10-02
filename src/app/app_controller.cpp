#include "app/app_controller.hpp"

#include <QMetaObject>
#include <QThread>

namespace revdash::app {
namespace {
QString text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
}

AppController::AppController(QObject* parent)
    : AppController(std::make_unique<core::EngineService>(), parent) {}

AppController::AppController(std::unique_ptr<core::EngineService> engine, QObject* parent)
    : QObject(parent), engine_(std::move(engine)), telemetry_model_(this), dtc_model_(this),
      finding_model_(this), session_model_(this), source_model_(this) {
    Q_ASSERT(engine_);
    engine_subscription_ = engine_->subscribe([this](const core::EngineEvent& event) { onEngineEvent(event); });
    telemetry_timer_.setInterval(50);
    telemetry_timer_.setTimerType(Qt::PreciseTimer);
    connect(&telemetry_timer_, &QTimer::timeout, this, &AppController::pollTelemetry);
    telemetry_timer_.start();
    chart_timer_.setInterval(100);
    chart_timer_.setTimerType(Qt::PreciseTimer);
    connect(&chart_timer_, &QTimer::timeout, this, &AppController::publishChartBatch);
    chart_timer_.start();
}

AppController::~AppController() {
    telemetry_timer_.stop();
    chart_timer_.stop();
    engine_subscription_.reset();
}

void AppController::setDarkTheme(bool value) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (dark_theme_ == value) return;
    dark_theme_ = value;
    emit darkThemeChanged();
}

void AppController::setImperial(bool value) {
    telemetry_model_.setUnitSystem(value ? TelemetryModel::UnitSystem::Imperial : TelemetryModel::UnitSystem::Metric);
}

void AppController::disconnectSource() {
    engine_->disconnect([this](core::Result<void> result) {
        if (result) return;
        const auto message = QString::fromStdString(result.error().message);
        QMetaObject::invokeMethod(this, [this, message] {
            last_error_ = message;
            emit lastErrorChanged();
        }, Qt::QueuedConnection);
    });
}

void AppController::onEngineEvent(const core::EngineEvent& event) {
    QMetaObject::invokeMethod(this, [this, event] { applyEngineEvent(event); }, Qt::QueuedConnection);
}

void AppController::applyEngineEvent(core::EngineEvent event) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (event.type == core::EngineEventType::ConnectionStateChanged) {
        const auto value = text(core::toString(event.connection_state));
        if (connection_state_ != value) { connection_state_ = value; emit connectionStateChanged(); }
    }
    if (event.type == core::EngineEventType::DiagnosticDataUpdated) {
        dtc_model_.setRecords(engine_->diagnosticSnapshot().dtcs);
    }
    if (event.type == core::EngineEventType::DiagnosticFindingsUpdated) {
        finding_model_.setFindings(engine_->diagnosticFindings());
    }
    if (event.error) {
        last_error_ = QString::fromStdString(event.error->message);
        emit lastErrorChanged();
    }
}

void AppController::pollTelemetry() {
    Q_ASSERT(QThread::currentThread() == thread());
    latest_snapshot_ = engine_->telemetrySnapshot();
    telemetry_model_.setSnapshot(latest_snapshot_);
}

void AppController::publishChartBatch() {
    Q_ASSERT(QThread::currentThread() == thread());
    for (std::size_t i = 0; i < core::kMetricCount; ++i) {
        const auto& sample = latest_snapshot_.samples[i];
        if (sample.isValid()) emit chartSample(static_cast<int>(i), sample.value);
    }
}
} // namespace revdash::app
