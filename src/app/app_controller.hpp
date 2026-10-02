#pragma once

#include <QObject>
#include <QTimer>

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
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    explicit AppController(std::unique_ptr<core::EngineService> engine, QObject* parent = nullptr);
    ~AppController() override;

    TelemetryModel* telemetryModel() noexcept { return &telemetry_model_; }
    DtcModel* dtcModel() noexcept { return &dtc_model_; }
    FindingModel* findingModel() noexcept { return &finding_model_; }
    SessionModel* sessionModel() noexcept { return &session_model_; }
    SourceModel* sourceModel() noexcept { return &source_model_; }
    bool darkTheme() const noexcept { return dark_theme_; }
    QString connectionState() const { return connection_state_; }
    QString lastError() const { return last_error_; }

    Q_INVOKABLE void setDarkTheme(bool value);
    Q_INVOKABLE void setImperial(bool value);
    Q_INVOKABLE void disconnectSource();

signals:
    void darkThemeChanged();
    void connectionStateChanged();
    void lastErrorChanged();
    void chartSample(int metricId, double value);

private:
    void onEngineEvent(const core::EngineEvent& event);
    void applyEngineEvent(core::EngineEvent event);
    void pollTelemetry();
    void publishChartBatch();

    std::unique_ptr<core::EngineService> engine_;
    core::SubscriptionToken engine_subscription_;
    TelemetryModel telemetry_model_;
    DtcModel dtc_model_;
    FindingModel finding_model_;
    SessionModel session_model_;
    SourceModel source_model_;
    QTimer telemetry_timer_;
    QTimer chart_timer_;
    core::TelemetrySnapshot latest_snapshot_{};
    QString connection_state_{QStringLiteral("Disconnected")};
    QString last_error_;
    bool dark_theme_{true};
};

} // namespace revdash::app
