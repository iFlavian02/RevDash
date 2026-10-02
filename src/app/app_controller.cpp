#include "app/app_controller.hpp"

#include <QMetaObject>
#include <QThread>

#include <algorithm>

#include "revdash/drivers/elm327.hpp"
#include "revdash/drivers/serial_transport.hpp"
#include "revdash/drivers/synthetic.hpp"

namespace revdash::app {
namespace {
QString text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

QString actionableError(const core::Error& error) {
    QString message = QString::fromStdString(error.message);
    if (!error.context.empty()) message += QStringLiteral(" (%1)").arg(QString::fromStdString(error.context));
    if (error.domain == core::ErrorDomain::Transport) {
        message += QObject::tr(". Check that the adapter is powered, the selected COM port is correct, and no other application is using it.");
    } else if (error.retryable) {
        message += QObject::tr(". RevDash will retry automatically; verify the vehicle ignition and adapter connection.");
    }
    return message;
}

core::SyntheticConfig syntheticConfig(int preset_index, quint32 seed) {
    core::SyntheticConfig config;
    config.deterministic_seed = seed;
    if (preset_index == 1) {
        config.initial_rpm = 900.0;
        config.noise_std_dev = 0.15;
        config.response_latency = std::chrono::milliseconds{15};
    } else if (preset_index == 2) {
        config.initial_rpm = 2200.0;
        config.ambient_temp_c = 24.0;
        config.response_latency = std::chrono::milliseconds{8};
    } else if (preset_index == 3) {
        config.inject_misfire = true;
        config.inject_vacuum_leak = true;
        config.noise_std_dev = 0.35;
        config.packet_dropout_prob = 0.02;
        config.include_second_ecu = true;
    }
    return config;
}
}

AppController::AppController(QObject* parent)
    : AppController(std::make_unique<core::EngineService>(), drivers::enumerateSerialPorts, parent) {}

AppController::AppController(std::unique_ptr<core::EngineService> engine, QObject* parent)
    : AppController(std::move(engine), drivers::enumerateSerialPorts, parent) {}

AppController::AppController(std::unique_ptr<core::EngineService> engine, SerialPortEnumerator port_enumerator, QObject* parent)
    : QObject(parent), engine_(std::move(engine)), telemetry_model_(this), dtc_model_(this),
      finding_model_(this), session_model_(this), source_model_(this), serial_port_model_(this),
      port_enumerator_(std::move(port_enumerator)) {
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
    source_status_timer_.setInterval(500);
    connect(&source_status_timer_, &QTimer::timeout, this, &AppController::pollSourceStatus);
    source_status_timer_.start();
    refreshSerialPorts();
}

AppController::~AppController() {
    telemetry_timer_.stop();
    chart_timer_.stop();
    source_status_timer_.stop();
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
    if (!sourceOperationsEnabled()) return;
    setSourceOperationBusy(true);
    engine_->disconnect([this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSourceOperation(std::move(result)); }, Qt::QueuedConnection); });
}

QStringList AppController::simulationPresets() const {
    return {tr("Balanced"), tr("City traffic"), tr("Highway cruise"), tr("Fault demo")};
}

void AppController::refreshSerialPorts() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!sourceOperationsEnabled() || !port_enumerator_) return;
    serial_port_model_.setPorts(port_enumerator_());
}

void AppController::connectSerial(const QString& port, int baud) {
    if (!sourceOperationsEnabled()) return;
    const auto normalized = drivers::normalizeSerialPortName(port.toStdString());
    if (!normalized || baud < 0 || !drivers::isSupportedSerialBaudRate(static_cast<std::uint32_t>(baud))) {
        last_error_ = tr("Choose an available COM port and a supported baud rate (9600, 38400, or 115200).");
        emit lastErrorChanged();
        return;
    }
    configured_port_ = QString::fromStdString(*normalized);
    configured_baud_ = baud;
    emit sourceConfigurationChanged();
    adapter_identity_.clear(); protocol_identity_.clear(); last_rtt_ms_ = 0; ewma_rtt_ms_ = 0; retry_count_ = 0;
    emit sourceStatusChanged();
    setSourceOperationBusy(true);
    last_error_.clear(); emit lastErrorChanged();
    const core::SerialConfig config{.port_name = *normalized, .baud_rate = static_cast<std::uint32_t>(baud)};
    engine_->setSource(std::make_unique<drivers::Elm327DataSource>(), [this, config](core::Result<void> result) mutable {
        if (!result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSourceOperation(std::move(result)); }, Qt::QueuedConnection); return; }
        engine_->connect(config, [this](core::Result<void> connected) { QMetaObject::invokeMethod(this, [this, connected = std::move(connected)]() mutable { finishSourceOperation(std::move(connected)); }, Qt::QueuedConnection); });
    });
}

void AppController::connectSynthetic(const QString& preset, quint32 seed) {
    if (!sourceOperationsEnabled()) return;
    const auto presets = simulationPresets();
    const auto preset_index = presets.indexOf(preset);
    if (preset_index < 0) {
        last_error_ = tr("Choose a valid simulation preset."); emit lastErrorChanged(); return;
    }
    configured_preset_ = preset;
    configured_seed_ = seed;
    emit sourceConfigurationChanged();
    adapter_identity_.clear(); protocol_identity_.clear(); last_rtt_ms_ = 0; ewma_rtt_ms_ = 0; retry_count_ = 0;
    emit sourceStatusChanged();
    setSourceOperationBusy(true);
    last_error_.clear(); emit lastErrorChanged();
    const auto config = syntheticConfig(preset_index, seed);
    engine_->setSource(std::make_unique<drivers::SyntheticDataSource>(), [this, config](core::Result<void> result) mutable {
        if (!result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSourceOperation(std::move(result)); }, Qt::QueuedConnection); return; }
        engine_->connect(config, [this](core::Result<void> connected) { QMetaObject::invokeMethod(this, [this, connected = std::move(connected)]() mutable { finishSourceOperation(std::move(connected)); }, Qt::QueuedConnection); });
    });
}

void AppController::setClearConfirmationPending(bool pending) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (clear_confirmation_pending_ == pending) return;
    clear_confirmation_pending_ = pending;
    emit sourceOperationsEnabledChanged();
}

void AppController::setSourceOperationBusy(bool busy) {
    if (source_operation_busy_ == busy) return;
    source_operation_busy_ = busy;
    emit sourceOperationsEnabledChanged();
}

void AppController::finishSourceOperation(core::Result<void> result) {
    Q_ASSERT(QThread::currentThread() == thread());
    setSourceOperationBusy(false);
    if (result) return;
    last_error_ = actionableError(result.error());
    ++error_count_;
    emit lastErrorChanged();
    emit sourceStatusChanged();
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
        last_error_ = actionableError(*event.error);
        ++error_count_;
        emit lastErrorChanged();
        emit sourceStatusChanged();
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

void AppController::pollSourceStatus() {
    engine_->querySourceStatus([this](core::SourceRuntimeStatus status) {
        QMetaObject::invokeMethod(this, [this, status = std::move(status)] {
            adapter_identity_ = QString::fromStdString(status.adapter_identity);
            protocol_identity_ = QString::fromStdString(status.protocol);
            last_rtt_ms_ = static_cast<int>(status.last_rtt.count());
            ewma_rtt_ms_ = static_cast<int>(status.ewma_rtt.count());
            retry_count_ = status.retry_count;
            error_count_ = std::max(error_count_, status.error_count);
            emit sourceStatusChanged();
        }, Qt::QueuedConnection);
    });
}
} // namespace revdash::app
