#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <memory>

#include "app/models.hpp"
#include "revdash/core/engine_service.hpp"

namespace revdash::app {

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(TelemetryModel* telemetryModel READ telemetryModel CONSTANT)
    Q_PROPERTY(DtcModel* dtcModel READ dtcModel CONSTANT)
    Q_PROPERTY(FindingModel* findingModel READ findingModel CONSTANT)
    Q_PROPERTY(SessionModel* sessionModel READ sessionModel CONSTANT)
    Q_PROPERTY(SourceModel* sourceModel READ sourceModel CONSTANT)
    Q_PROPERTY(SerialPortModel* serialPortModel READ serialPortModel CONSTANT)
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool sourceOperationsEnabled READ sourceOperationsEnabled NOTIFY sourceOperationsEnabledChanged)
    Q_PROPERTY(bool clearConfirmationPending READ clearConfirmationPending NOTIFY sourceOperationsEnabledChanged)
    Q_PROPERTY(QString adapterIdentity READ adapterIdentity NOTIFY sourceStatusChanged)
    Q_PROPERTY(QString protocolIdentity READ protocolIdentity NOTIFY sourceStatusChanged)
    Q_PROPERTY(int lastRttMs READ lastRttMs NOTIFY sourceStatusChanged)
    Q_PROPERTY(int ewmaRttMs READ ewmaRttMs NOTIFY sourceStatusChanged)
    Q_PROPERTY(quint32 retryCount READ retryCount NOTIFY sourceStatusChanged)
    Q_PROPERTY(quint32 errorCount READ errorCount NOTIFY sourceStatusChanged)
    Q_PROPERTY(double actualPollRate READ actualPollRate NOTIFY telemetryHealthChanged)
    Q_PROPERTY(qint64 latestSampleAgeMs READ latestSampleAgeMs NOTIFY telemetryHealthChanged)
    Q_PROPERTY(quint64 sourceQueueDrops READ sourceQueueDrops NOTIFY telemetryHealthChanged)
    Q_PROPERTY(quint64 recorderQueueDrops READ recorderQueueDrops NOTIFY telemetryHealthChanged)
    Q_PROPERTY(QStringList simulationPresets READ simulationPresets CONSTANT)
    Q_PROPERTY(QString configuredPort READ configuredPort NOTIFY sourceConfigurationChanged)
    Q_PROPERTY(int configuredBaud READ configuredBaud NOTIFY sourceConfigurationChanged)
    Q_PROPERTY(quint32 configuredSeed READ configuredSeed NOTIFY sourceConfigurationChanged)
    Q_PROPERTY(QString configuredPreset READ configuredPreset NOTIFY sourceConfigurationChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    explicit AppController(std::unique_ptr<core::EngineService> engine, QObject* parent = nullptr);
    using SerialPortEnumerator = std::function<std::vector<drivers::SerialPortInfo>()>;
    AppController(std::unique_ptr<core::EngineService> engine, SerialPortEnumerator port_enumerator, QObject* parent = nullptr);
    ~AppController() override;

    TelemetryModel* telemetryModel() noexcept { return &telemetry_model_; }
    DtcModel* dtcModel() noexcept { return &dtc_model_; }
    FindingModel* findingModel() noexcept { return &finding_model_; }
    SessionModel* sessionModel() noexcept { return &session_model_; }
    SourceModel* sourceModel() noexcept { return &source_model_; }
    SerialPortModel* serialPortModel() noexcept { return &serial_port_model_; }
    bool darkTheme() const noexcept { return dark_theme_; }
    QString connectionState() const { return connection_state_; }
    QString lastError() const { return last_error_; }
    bool sourceOperationsEnabled() const noexcept { return !source_operation_busy_ && !clear_confirmation_pending_; }
    bool clearConfirmationPending() const noexcept { return clear_confirmation_pending_; }
    QString adapterIdentity() const { return adapter_identity_; }
    QString protocolIdentity() const { return protocol_identity_; }
    int lastRttMs() const noexcept { return last_rtt_ms_; }
    int ewmaRttMs() const noexcept { return ewma_rtt_ms_; }
    quint32 retryCount() const noexcept { return retry_count_; }
    quint32 errorCount() const noexcept { return error_count_; }
    double actualPollRate() const noexcept { return actual_poll_rate_; }
    qint64 latestSampleAgeMs() const noexcept { return latest_sample_age_ms_; }
    quint64 sourceQueueDrops() const noexcept { return source_queue_drops_; }
    quint64 recorderQueueDrops() const noexcept { return recorder_queue_drops_; }
    QStringList simulationPresets() const;
    QString configuredPort() const { return configured_port_; }
    int configuredBaud() const noexcept { return configured_baud_; }
    quint32 configuredSeed() const noexcept { return configured_seed_; }
    QString configuredPreset() const { return configured_preset_; }

    Q_INVOKABLE void setDarkTheme(bool value);
    Q_INVOKABLE void setImperial(bool value);
    Q_INVOKABLE void disconnectSource();
    Q_INVOKABLE void refreshSerialPorts();
    Q_INVOKABLE void connectSerial(const QString& port, int baud);
    Q_INVOKABLE void connectSynthetic(const QString& preset, quint32 seed);
    Q_INVOKABLE void setClearConfirmationPending(bool pending);

signals:
    void darkThemeChanged();
    void connectionStateChanged();
    void lastErrorChanged();
    void sourceOperationsEnabledChanged();
    void sourceStatusChanged();
    void sourceConfigurationChanged();
    void telemetryHealthChanged();
    void presentationUnitsChanged();
    void chartSample(int metricId, double value);

private:
    void onEngineEvent(const core::EngineEvent& event);
    void applyEngineEvent(core::EngineEvent event);
    void pollTelemetry();
    void publishChartBatch();
    void pollSourceStatus();
    void finishSourceOperation(core::Result<void> result);
    void setSourceOperationBusy(bool busy);

    std::unique_ptr<core::EngineService> engine_;
    core::SubscriptionToken engine_subscription_;
    TelemetryModel telemetry_model_;
    DtcModel dtc_model_;
    FindingModel finding_model_;
    SessionModel session_model_;
    SourceModel source_model_;
    SerialPortModel serial_port_model_;
    SerialPortEnumerator port_enumerator_;
    QTimer telemetry_timer_;
    QTimer chart_timer_;
    QTimer source_status_timer_;
    QElapsedTimer poll_rate_clock_;
    core::TelemetrySnapshot latest_snapshot_{};
    QString connection_state_{QStringLiteral("Disconnected")};
    QString last_error_;
    QString adapter_identity_;
    QString protocol_identity_;
    QString configured_port_;
    QString configured_preset_{QStringLiteral("Balanced")};
    int configured_baud_{38400};
    quint32 configured_seed_{12345};
    int last_rtt_ms_{0};
    int ewma_rtt_ms_{0};
    quint32 retry_count_{0};
    quint32 error_count_{0};
    quint64 previous_source_packets_{0};
    quint64 source_queue_drops_{0};
    quint64 recorder_queue_drops_{0};
    double actual_poll_rate_{0.0};
    qint64 latest_sample_age_ms_{0};
    bool dark_theme_{true};
    bool source_operation_busy_{false};
    bool clear_confirmation_pending_{false};
};

} // namespace revdash::app
