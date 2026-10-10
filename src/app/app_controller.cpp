#include "app/app_controller.hpp"

#include <QMetaObject>
#include <QGuiApplication>
#include <QClipboard>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

#include "revdash/drivers/elm327.hpp"
#include "revdash/drivers/serial_transport.hpp"
#include "revdash/drivers/synthetic.hpp"
#include "revdash/drivers/playback.hpp"
#include "revdash/session/csv_exporter.hpp"

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

QString humanSize(qint64 bytes) {
    if (bytes < 1024) return QObject::tr("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QObject::tr("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QObject::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

SessionEntry inspectSession(const QFileInfo& info) {
    SessionEntry entry{.name = info.completeBaseName(), .path = info.absoluteFilePath(),
        .fileSize = humanSize(info.size()), .fileSizeBytes = info.size(), .recoverable = info.suffix() == QStringLiteral("partial")};
    std::ifstream input{std::filesystem::path{info.absoluteFilePath().toStdWString()}};
    std::string line;
    bool header_seen = false;
    while (std::getline(input, line)) {
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) continue;
        entry.durationUs = std::max(entry.durationUs, static_cast<qint64>(parsed.value("elapsed_us", std::int64_t{0})));
        const auto type = parsed.value("type", std::string{});
        if (type == "header" && !header_seen) {
            header_seen = true;
            entry.startedAt = QString::fromStdString(parsed.value("utc_start", std::string{}));
            entry.source = QString::fromStdString(parsed.value("source_type", std::string{}));
            if (const auto vehicles = parsed.find("vehicle_metadata"); vehicles != parsed.end() && vehicles->is_array()) {
                QStringList vins;
                for (const auto& vehicle : *vehicles) {
                    const auto vin = QString::fromStdString(vehicle.value("vin", std::string{}));
                    if (!vin.isEmpty()) vins.push_back(vin);
                }
                vins.removeDuplicates();
                entry.vehicle = vins.join(QStringLiteral(", "));
            }
        } else if (type == "dtc") {
            ++entry.dtcCount;
        } else if (type == "footer") {
            if (const auto statistics = parsed.find("statistics"); statistics != parsed.end() && statistics->is_object())
                entry.dtcCount = statistics->value("dtcs", entry.dtcCount);
        }
    }
    if (entry.startedAt.isEmpty()) entry.startedAt = info.lastModified().toUTC().toString(Qt::ISODate);
    if (entry.source.isEmpty()) entry.source = QObject::tr("Unknown");
    if (entry.vehicle.isEmpty()) entry.vehicle = QObject::tr("Vehicle not identified");
    return entry;
}
}

AppController::AppController(QObject* parent)
    : AppController(std::make_unique<core::EngineService>(), drivers::enumerateSerialPorts, parent) {}

AppController::AppController(std::unique_ptr<core::EngineService> engine, QObject* parent)
    : AppController(std::move(engine), drivers::enumerateSerialPorts, parent) {}

AppController::AppController(std::unique_ptr<core::EngineService> engine, SerialPortEnumerator port_enumerator, QObject* parent)
    : QObject(parent), engine_(std::move(engine)), telemetry_model_(this), dtc_model_(this),
      finding_model_(this), raw_diagnostic_model_(this), session_model_(this), dtc_lookup_model_(this), source_model_(this), serial_port_model_(this),
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
    loadSettings();
    refreshSessions();
    loadDtcDatabase();
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
    if (settings_) settings_->setValue(QStringLiteral("appearance/darkTheme"), value);
    emit darkThemeChanged();
}

void AppController::setImperial(bool value) {
    telemetry_model_.setUnitSystem(value ? TelemetryModel::UnitSystem::Imperial : TelemetryModel::UnitSystem::Metric);
    if (settings_) settings_->setValue(QStringLiteral("appearance/imperial"), value);
    emit presentationUnitsChanged();
}

QString AppController::validatedDirectory(const QString& stored, const QString& fallbackLeaf) const {
    auto base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString candidate = stored.trimmed();
    if (candidate.isEmpty()) candidate = QDir(base).filePath(QStringLiteral("RevDash/%1").arg(fallbackLeaf));
    const auto ensureDirectory = [](const QString& path) {
        const QFileInfo info(path);
        return info.isDir() || (!info.exists() && QDir().mkpath(path) && QFileInfo(path).isDir());
    };
    if (!ensureDirectory(candidate)) {
        candidate = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(fallbackLeaf);
        if (!ensureDirectory(candidate)) {
            candidate = QDir(QDir::tempPath()).filePath(QStringLiteral("RevDash/%1").arg(fallbackLeaf));
            static_cast<void>(ensureDirectory(candidate));
        }
    }
    return QDir(candidate).absolutePath();
}

void AppController::loadSettings() {
    settings_ = std::make_unique<QSettings>();
    dark_theme_ = settings_->value(QStringLiteral("appearance/darkTheme"), true).toBool();
    telemetry_model_.setUnitSystem(settings_->value(QStringLiteral("appearance/imperial"), false).toBool()
        ? TelemetryModel::UnitSystem::Imperial : TelemetryModel::UnitSystem::Metric);
    session_path_ = validatedDirectory(settings_->value(QStringLiteral("paths/sessions")).toString(), QStringLiteral("Sessions"));
    export_path_ = validatedDirectory(settings_->value(QStringLiteral("paths/exports")).toString(), QStringLiteral("Exports"));
    preferred_port_ = settings_->value(QStringLiteral("connection/port")).toString();
    preferred_baud_ = settings_->value(QStringLiteral("connection/baud"), 38400).toInt();
    if (!drivers::isSupportedSerialBaudRate(static_cast<std::uint32_t>(std::max(0, preferred_baud_)))) preferred_baud_ = 38400;
    dtc_database_path_ = settings_->value(QStringLiteral("paths/dtcDatabase"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("dtc.sqlite"))).toString();
    settings_->setValue(QStringLiteral("paths/sessions"), session_path_);
    settings_->setValue(QStringLiteral("paths/exports"), export_path_);
}

void AppController::setSessionPath(const QString& value) {
    const QFileInfo requested(value.trimmed());
    const auto resolved = requested.exists() && !requested.isDir() && QFileInfo(session_path_).isDir()
        ? session_path_ : validatedDirectory(value, QStringLiteral("Sessions"));
    if (session_path_ == resolved) return; session_path_ = resolved; settings_->setValue(QStringLiteral("paths/sessions"), resolved); refreshSessions(); emit settingsChanged();
}
void AppController::setExportPath(const QString& value) {
    const QFileInfo requested(value.trimmed());
    const auto resolved = requested.exists() && !requested.isDir() && QFileInfo(export_path_).isDir()
        ? export_path_ : validatedDirectory(value, QStringLiteral("Exports"));
    if (export_path_ == resolved) return; export_path_ = resolved; settings_->setValue(QStringLiteral("paths/exports"), resolved); emit settingsChanged();
}
void AppController::setPreferredPort(const QString& value) { if (preferred_port_ == value) return; preferred_port_=value; settings_->setValue(QStringLiteral("connection/port"), value); emit settingsChanged(); }
void AppController::setPreferredBaud(int value) { if (value < 0 || !drivers::isSupportedSerialBaudRate(static_cast<std::uint32_t>(value)) || preferred_baud_ == value) return; preferred_baud_=value; settings_->setValue(QStringLiteral("connection/baud"), value); emit settingsChanged(); }
void AppController::setDtcDatabasePath(const QString& value) { if (dtc_database_path_ == value) return; dtc_database_path_=QDir::cleanPath(value); settings_->setValue(QStringLiteral("paths/dtcDatabase"), dtc_database_path_); loadDtcDatabase(); emit settingsChanged(); }

void AppController::refreshSessions() {
    QList<SessionEntry> sessions;
    QDir directory(session_path_);
    const auto files = directory.entryInfoList({QStringLiteral("*.jsonl"), QStringLiteral("*.partial")}, QDir::Files, QDir::Time);
    sessions.reserve(files.size());
    for (const auto& file : files) sessions.push_back(inspectSession(file));
    session_model_.setSessions(std::move(sessions));
}

void AppController::selectSession(int row) {
    const auto* entry = session_model_.entry(row);
    if (!entry) return;
    selected_session_path_ = entry->path;
    playback_duration_us_ = entry->durationUs;
    playback_position_us_ = 0;
    session_action_message_ = entry->recoverable ? tr("Recoverable partial sessions cannot be played or exported until repaired.") : QString{};
    emit playbackChanged();
}

void AppController::playSession() {
    if (selected_session_path_.isEmpty() || selected_session_path_.endsWith(QStringLiteral(".partial"), Qt::CaseInsensitive)) return;
    if (configured_physical_source_ || connection_state_ != QStringLiteral("Ready") || playback_state_ == QStringLiteral("Stopped")) {
        configured_physical_source_ = false;
        setSourceOperationBusy(true);
        auto path = selected_session_path_.toStdString();
        engine_->setSource(std::make_unique<drivers::PlaybackDataSource>(), [this, path = std::move(path)](core::Result<void> result) mutable {
            if (!result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSourceOperation(std::move(result)); }, Qt::QueuedConnection); return; }
            engine_->connect(core::PlaybackConfig{.session_file_path=path,.speed_multiplier=playback_speed_}, [this](core::Result<void> connected) mutable {
                if (!connected) { QMetaObject::invokeMethod(this, [this, connected=std::move(connected)]() mutable { finishSourceOperation(std::move(connected)); }, Qt::QueuedConnection); return; }
                engine_->startPlayback([this](core::Result<void> started) { QMetaObject::invokeMethod(this, [this, started=std::move(started)]() mutable { setSourceOperationBusy(false); finishPlaybackCommand(std::move(started), QStringLiteral("Playing")); }, Qt::QueuedConnection); });
            });
        });
        return;
    }
    engine_->startPlayback([this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this,result=std::move(result)]() mutable { finishPlaybackCommand(std::move(result), QStringLiteral("Playing")); }, Qt::QueuedConnection); });
}
void AppController::pauseSession() { engine_->pausePlayback([this](core::Result<void> r){ QMetaObject::invokeMethod(this,[this,r=std::move(r)]() mutable { finishPlaybackCommand(std::move(r),QStringLiteral("Paused")); },Qt::QueuedConnection); }); }
void AppController::stepSession() { engine_->stepPlayback([this](core::Result<void> r){ QMetaObject::invokeMethod(this,[this,r=std::move(r)]() mutable { finishPlaybackCommand(std::move(r),QStringLiteral("Paused")); },Qt::QueuedConnection); }); }
void AppController::stopSession() { engine_->stopPlayback([this](core::Result<void> r){ QMetaObject::invokeMethod(this,[this,r=std::move(r)]() mutable { playback_position_us_=0; finishPlaybackCommand(std::move(r),QStringLiteral("Stopped")); },Qt::QueuedConnection); }); }
void AppController::seekSession(qint64 targetUs) { targetUs=std::clamp<qint64>(targetUs,0,playback_duration_us_); engine_->seekPlayback(std::chrono::microseconds{targetUs},[this,targetUs](core::Result<void> r){ QMetaObject::invokeMethod(this,[this,targetUs,r=std::move(r)]() mutable { if(r)playback_position_us_=targetUs; finishPlaybackCommand(std::move(r)); },Qt::QueuedConnection); }); }
void AppController::setSessionSpeed(double multiplier) { if (multiplier!=0.5&&multiplier!=1.0&&multiplier!=2.0&&multiplier!=5.0)return; playback_speed_=multiplier; engine_->setPlaybackSpeed(multiplier,[this](core::Result<void> r){ QMetaObject::invokeMethod(this,[this,r=std::move(r)]() mutable { finishPlaybackCommand(std::move(r)); },Qt::QueuedConnection); }); emit playbackChanged(); }
void AppController::finishPlaybackCommand(core::Result<void> result, const QString& successState) { if(!result){session_action_message_=actionableError(result.error());}else{if(!successState.isEmpty())playback_state_=successState;session_action_message_.clear();}emit playbackChanged(); }

void AppController::exportSelectedSession(const QString& destination, int preset) {
    if (selected_session_path_.isEmpty() || selected_session_path_.endsWith(QStringLiteral(".partial"), Qt::CaseInsensitive)) return;
    auto output=destination.trimmed(); if(output.isEmpty()) output=QDir(export_path_).filePath(QFileInfo(selected_session_path_).completeBaseName()+QStringLiteral(".csv"));
    if(!output.endsWith(QStringLiteral(".csv"),Qt::CaseInsensitive))output+=QStringLiteral(".csv");
    session::CsvExportOptions options{.preset=static_cast<session::CsvPreset>(std::clamp(preset,0,2)),.units=imperial()?session::UnitSystem::Imperial:session::UnitSystem::Metric};
    const auto result=session::exportSessionCsv(std::filesystem::path{selected_session_path_.toStdWString()},std::filesystem::path{output.toStdWString()},options);
    session_action_message_=result?tr("Exported %1").arg(QDir::toNativeSeparators(output)):actionableError(result.error()); emit playbackChanged();
}

void AppController::loadDtcDatabase() {
    dtc_database_.reset(); dtc_lookup_model_.clear();
    auto result=diagnostics::DtcDatabase::openReadOnly(std::filesystem::path{dtc_database_path_.toStdWString()},diagnostics::DtcDatabaseKind::Production);
    if(!result){dtc_database_status_=tr("Production DTC database unavailable. Choose a licensed database file to enable lookup.");engine_->setDtcDatabase({});}
    else{dtc_database_=std::make_shared<diagnostics::DtcDatabase>(std::move(*result));dtc_database_status_=tr("DTC database loaded — %1").arg(QString::fromStdString(dtc_database_->metadata().source_version));engine_->setDtcDatabase(dtc_database_);}
    emit dtcLookupChanged();
}
void AppController::lookupDtc(const QString& query) {
    dtc_lookup_model_.clear();
    if(!dtc_database_){dtc_lookup_message_=dtc_database_status_;emit dtcLookupChanged();return;}
    const auto normalized=query.trimmed(); if(normalized.isEmpty()){dtc_lookup_message_=tr("Enter a five-character DTC or diagnostic keywords.");emit dtcLookupChanged();return;}
    std::vector<diagnostics::DtcDefinition> definitions;
    if(normalized.size()==5 && (normalized.startsWith(QLatin1Char('P'),Qt::CaseInsensitive)||normalized.startsWith(QLatin1Char('B'),Qt::CaseInsensitive)||normalized.startsWith(QLatin1Char('C'),Qt::CaseInsensitive)||normalized.startsWith(QLatin1Char('U'),Qt::CaseInsensitive))){auto r=dtc_database_->lookupExact(normalized.toStdString());if(r&&r->known)definitions.push_back(*r);else if(!r){dtc_lookup_message_=actionableError(r.error());emit dtcLookupChanged();return;}}
    else{auto r=dtc_database_->searchKeywords(normalized.toStdString());if(!r){dtc_lookup_message_=actionableError(r.error());emit dtcLookupChanged();return;}definitions=std::move(*r);}
    dtc_lookup_message_=definitions.empty()?tr("No matching diagnostic codes found."):tr("%1 result(s)").arg(definitions.size());dtc_lookup_model_.setDefinitions(std::move(definitions));emit dtcLookupChanged();
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
    emit simulatorStateChanged();
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
    simulation_state_.ambient_temp_c = config.ambient_temp_c;
    simulation_state_.physical.rpm = config.initial_rpm;
    simulation_state_.faults = {.misfire = config.inject_misfire, .vacuum_leak = config.inject_vacuum_leak,
        .stuck_open_thermostat = config.inject_thermostat_fault, .sensor_noise_std_dev = config.noise_std_dev,
        .packet_dropout_probability = config.packet_dropout_prob};
    emit simulatorStateChanged();
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

double AppController::observedMetric(core::MetricId metric) const noexcept {
    const auto& sample = latest_snapshot_.get(metric);
    return sample.isValid() ? sample.value : std::numeric_limits<double>::quiet_NaN();
}
double AppController::simulationObservedRpm() const noexcept { return observedMetric(core::MetricId::Rpm); }
double AppController::simulationObservedSpeed() const noexcept { return observedMetric(core::MetricId::VehicleSpeed); }
double AppController::simulationObservedCoolant() const noexcept { return observedMetric(core::MetricId::CoolantTemp); }
double AppController::simulationObservedMap() const noexcept { return observedMetric(core::MetricId::Map); }

void AppController::finishSimulationCommand(core::Result<void> result) {
    if (result) return;
    last_error_ = actionableError(result.error());
    ++error_count_;
    emit lastErrorChanged();
    emit sourceStatusChanged();
}

void AppController::setSimulationIgnition(bool enabled) {
    if (!simulatorControlsEnabled()) return;
    simulation_state_.ignition_on = enabled;
    if (!enabled) simulation_state_.engine_running = false;
    emit simulatorStateChanged();
    engine_->setSimulationIgnition(enabled, [this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
}
void AppController::setSimulationEngineRunning(bool running) {
    if (!simulatorControlsEnabled() || (running && !simulation_state_.ignition_on)) return;
    simulation_state_.engine_running = running;
    emit simulatorStateChanged();
    engine_->setSimulationEngineRunning(running, [this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
}
void AppController::setSimulationThrottle(double percent) {
    if (!simulatorControlsEnabled()) return;
    percent = std::clamp(percent, 0.0, 100.0);
    simulation_state_.physical.throttle_percent = percent;
    emit simulatorStateChanged();
    engine_->setSimulationThrottle(percent, [this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
}
void AppController::setSimulationAmbientTemperature(double celsius) {
    if (!simulatorControlsEnabled()) return;
    celsius = std::clamp(celsius, -40.0, 80.0);
    simulation_state_.ambient_temp_c = celsius;
    emit simulatorStateChanged();
    engine_->setSimulationAmbientTemperature(celsius, [this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
}
void AppController::setSimulationFaults(bool misfire, bool vacuumLeak, bool thermostatFault, double noise, double dropout) {
    if (!simulatorControlsEnabled()) return;
    simulation_state_.faults = {.misfire = misfire, .vacuum_leak = vacuumLeak, .stuck_open_thermostat = thermostatFault,
        .sensor_noise_std_dev = std::clamp(noise, 0.0, 25.0), .packet_dropout_probability = std::clamp(dropout, 0.0, 1.0)};
    emit simulatorStateChanged();
    engine_->setSimulationFaults(simulation_state_.faults, [this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
}
void AppController::resetSimulation() {
    if (!simulatorControlsEnabled()) return;
    engine_->resetSimulation([this](core::Result<void> result) { QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finishSimulationCommand(std::move(result)); }, Qt::QueuedConnection); });
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
        emit simulatorStateChanged();
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
    engine_->querySimulationState([this](std::optional<drivers::SimulationRuntimeState> state) {
        if (!state) return;
        QMetaObject::invokeMethod(this, [this, state = *state] {
            simulation_state_ = state;
            emit simulatorStateChanged();
        }, Qt::QueuedConnection);
    });
    engine_->queryPlaybackState([this](std::optional<core::PlaybackRuntimeStatus> state) {
        if (!state) return;
        QMetaObject::invokeMethod(this, [this, state = *state] {
            playback_position_us_ = state.position.count();
            playback_duration_us_ = state.duration.count();
            switch (state.state) {
                case drivers::PlaybackState::Playing: playback_state_ = QStringLiteral("Playing"); break;
                case drivers::PlaybackState::Paused: playback_state_ = QStringLiteral("Paused"); break;
                case drivers::PlaybackState::Stopped: playback_state_ = QStringLiteral("Stopped"); break;
            }
            emit playbackChanged();
        }, Qt::QueuedConnection);
    });
}
} // namespace revdash::app
