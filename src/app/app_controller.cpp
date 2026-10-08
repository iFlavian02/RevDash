#include "app/app_controller.hpp"

#include <QMetaObject>
#include <QGuiApplication>
#include <QClipboard>
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
      finding_model_(this), raw_diagnostic_model_(this), session_model_(this), source_model_(this), serial_port_model_(this),
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
    clear_countdown_timer_.setInterval(250);
    connect(&clear_countdown_timer_, &QTimer::timeout, this, &AppController::updateClearCountdown);
    poll_rate_clock_.start();
    refreshSerialPorts();
}

AppController::~AppController() {
    telemetry_timer_.stop();
    chart_timer_.stop();
    source_status_timer_.stop();
    clear_countdown_timer_.stop();
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
    emit presentationUnitsChanged();
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
    configured_physical_source_ = true;
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
    configured_physical_source_ = false;
    configured_seed_ = seed;
    emit sourceConfigurationChanged();
    adapter_identity_.clear(); protocol_identity_.clear(); last_rtt_ms_ = 0; ewma_rtt_ms_ = 0; retry_count_ = 0;
    emit sourceStatusChanged();
    setSourceOperationBusy(true);
    last_error_.clear(); emit lastErrorChanged();
    const auto config = syntheticConfig(preset_index, seed);
    engine_->setSource(std::make_unique<drivers::SyntheticDataSource>(), [this, config](core::Result<void> result) mutable {
        if (!result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSourceOperation(std::move(result)); }, Qt::QueuedConnection); return; }
        engine_->setSupportedPids({0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x11, 0x2F, 0x42, 0x46});
        engine_->connect(config, [this](core::Result<void> connected) { QMetaObject::invokeMethod(this, [this, connected = std::move(connected)]() mutable { finishSourceOperation(std::move(connected)); }, Qt::QueuedConnection); });
    });
}

void AppController::setClearConfirmationPending(bool pending) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (clear_confirmation_pending_ == pending) return;
    clear_confirmation_pending_ = pending;
    emit sourceOperationsEnabledChanged();
    emit diagnosticStateChanged();
}

bool AppController::clearDtcEnabled() const noexcept {
    return configured_physical_source_ && connection_state_ == QStringLiteral("Ready") && !diagnostic_busy_ && !clear_confirmation_pending_;
}

QString AppController::clearPreconditionStatus() const {
    if (connection_state_ != QStringLiteral("Ready")) return tr("Connect a physical ELM327 source before clearing diagnostic information.");
    if (!configured_physical_source_) return tr("Unavailable for simulation and playback sources.");
    if (diagnostic_busy_) return tr("Wait for the active diagnostic operation to finish.");
    return tr("Ready to validate vehicle identity, fresh speed data, and stationary state.");
}

void AppController::setDiagnosticBusy(bool busy) {
    if (diagnostic_busy_ == busy) return;
    diagnostic_busy_ = busy;
    emit diagnosticStateChanged();
}

void AppController::refreshDiagnosticModels() {
    const auto snapshot = engine_->diagnosticSnapshot();
    dtc_model_.setRecords(snapshot.dtcs);
    finding_model_.setFindings(engine_->diagnosticFindings());
    raw_diagnostic_model_.setLines(engine_->recentDiagnosticLines());
}

void AppController::scanDiagnostics() {
    if (diagnostic_busy_ || clear_confirmation_pending_ || connection_state_ != QStringLiteral("Ready")) return;
    setDiagnosticBusy(true);
    clear_result_.clear(); emit diagnosticStateChanged();
    engine_->scan([this](core::Result<core::DiagnosticSnapshot> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
            setDiagnosticBusy(false);
            if (!result) { last_error_ = actionableError(result.error()); ++error_count_; emit lastErrorChanged(); emit sourceStatusChanged(); return; }
            refreshDiagnosticModels();
        }, Qt::QueuedConnection);
    });
}

void AppController::prepareClearDiagnostics() {
    if (!clearDtcEnabled()) return;
    setDiagnosticBusy(true);
    clear_result_.clear(); emit diagnosticStateChanged();
    engine_->prepareClear([this](core::Result<core::ClearDtcPreparation> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
            setDiagnosticBusy(false);
            if (!result) { clear_result_ = actionableError(result.error()); emit diagnosticStateChanged(); return; }
            clear_warning_ = QString::fromStdString(result->warning);
            clear_confirmation_token_ = QString::fromStdString(result->confirmation_token);
            clear_expires_at_ = result->expires_at;
            setClearConfirmationPending(true);
            updateClearCountdown();
            clear_countdown_timer_.start();
        }, Qt::QueuedConnection);
    });
}

void AppController::confirmClearDiagnostics(const QString& token) {
    if (!clear_confirmation_pending_ || diagnostic_busy_ || clear_countdown_seconds_ <= 0) return;
    setDiagnosticBusy(true);
    clear_countdown_timer_.stop();
    engine_->confirmClear(token.toStdString(), [this](core::Result<core::Mode04AuditRecord> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
            setDiagnosticBusy(false);
            setClearConfirmationPending(false);
            clear_confirmation_token_.clear();
            clear_countdown_seconds_ = 0;
            if (!result) clear_result_ = actionableError(result.error());
            else clear_result_ = result->post_clear_rescan_completed
                ? tr("Clear accepted. Automatic rescan completed with %1 remaining DTC(s).").arg(result->post_clear_dtcs.size())
                : tr("Clear accepted, but the automatic rescan did not complete.");
            refreshDiagnosticModels();
            emit diagnosticStateChanged();
        }, Qt::QueuedConnection);
    });
}

void AppController::cancelClearDiagnostics() {
    clear_countdown_timer_.stop();
    clear_confirmation_token_.clear();
    clear_countdown_seconds_ = 0;
    setClearConfirmationPending(false);
}

void AppController::updateClearCountdown() {
    if (!clear_confirmation_pending_) return;
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(clear_expires_at_ - core::MonotonicClock::now());
    const auto value = static_cast<int>(std::max<std::int64_t>(0, remaining.count() + 1));
    if (clear_countdown_seconds_ == value) return;
    clear_countdown_seconds_ = value;
    if (value == 0) { clear_countdown_timer_.stop(); clear_result_ = tr("Confirmation token expired. Close this dialog and prepare again."); }
    emit diagnosticStateChanged();
}

void AppController::setRawTerminalPaused(bool paused) {
    raw_diagnostic_model_.setPaused(paused);
    if (!paused) raw_diagnostic_model_.setLines(engine_->recentDiagnosticLines());
}
void AppController::setRawTerminalHexFilter(const QString& filter) { raw_diagnostic_model_.setHexFilter(filter); }
void AppController::copyRawTerminal() { if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(raw_diagnostic_model_.copyText()); }

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
        emit diagnosticStateChanged();
    }
    if (event.type == core::EngineEventType::DiagnosticDataUpdated) {
        refreshDiagnosticModels();
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
    latest_sample_age_ms_ = telemetry_model_.maximumSampleAgeMs();
    emit telemetryHealthChanged();
}

void AppController::publishChartBatch() {
    Q_ASSERT(QThread::currentThread() == thread());
    for (std::size_t i = 0; i < core::kMetricCount; ++i) {
        const auto& sample = latest_snapshot_.samples[i];
        if (sample.isValid()) emit chartSample(static_cast<int>(i), telemetry_model_.presentationValue(sample.metric_id, sample.value));
    }
}

void AppController::pollSourceStatus() {
    const auto source_health = engine_->sourceQueueHealth();
    const auto recorder_health = engine_->recorderQueueHealth();
    const auto elapsed_ms = poll_rate_clock_.restart();
    if (elapsed_ms > 0) {
        const auto packet_delta = source_health.popped >= previous_source_packets_
            ? source_health.popped - previous_source_packets_ : 0;
        actual_poll_rate_ = static_cast<double>(packet_delta) * 1000.0 / static_cast<double>(elapsed_ms);
    }
    previous_source_packets_ = source_health.popped;
    source_queue_drops_ = source_health.dropped;
    recorder_queue_drops_ = recorder_health.dropped;
    emit telemetryHealthChanged();
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
