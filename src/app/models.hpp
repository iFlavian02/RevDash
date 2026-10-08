#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>

#include "revdash/core/diagnostic_types.hpp"
#include "revdash/core/telemetry_types.hpp"
#include "revdash/drivers/serial_transport.hpp"

namespace revdash::app {

class TelemetryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(UnitSystem unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)

public:
    enum class UnitSystem { Metric, Imperial };
    Q_ENUM(UnitSystem)
    enum Role {
        MetricIdRole = Qt::UserRole + 1,
        NameRole,
        ValueRole,
        UnitRole,
        QualityRole,
        ValidRole,
        SampleAgeMsRole,
        StateLabelRole
    };

    explicit TelemetryModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    UnitSystem unitSystem() const noexcept { return unit_system_; }
    void setUnitSystem(UnitSystem value);
    void setSnapshot(const core::TelemetrySnapshot& snapshot);
    [[nodiscard]] double presentationValue(core::MetricId id, double value) const;
    [[nodiscard]] qint64 maximumSampleAgeMs() const;

signals:
    void unitSystemChanged();

private:
    core::TelemetrySnapshot snapshot_{};
    UnitSystem unit_system_{UnitSystem::Metric};
};

class DtcModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        CodeRole = Qt::UserRole + 1, StatusRole, SeverityRole, DescriptionRole, EcuRole,
        GroupRole, AdvisoryRole, FailurePointsRole, HasFreezeFrameRole, FreezeFrameTitleRole,
        FreezeFrameSamplesRole
    };
    explicit DtcModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setRecords(std::vector<core::DtcRecord> records);
private:
    std::vector<core::DtcRecord> records_;
};

class FindingModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        RuleIdRole = Qt::UserRole + 1, TitleRole, DescriptionRole, SeverityRole, ActiveRole,
        StatusRole, EvidenceRole, LimitationsRole
    };
    explicit FindingModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setFindings(std::vector<core::DiagnosticFinding> findings);
private:
    std::vector<core::DiagnosticFinding> findings_;
};

class RawDiagnosticModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(QString hexFilter READ hexFilter WRITE setHexFilter NOTIFY hexFilterChanged)
public:
    enum Role { LineRole = Qt::UserRole + 1 };
    static constexpr qsizetype kMaximumLines = 500;

    explicit RawDiagnosticModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool paused() const noexcept { return paused_; }
    QString hexFilter() const { return hex_filter_; }
    void setPaused(bool value);
    void setHexFilter(const QString& value);
    void setLines(const std::vector<std::string>& lines);
    Q_INVOKABLE QString copyText() const;

signals:
    void pausedChanged();
    void hexFilterChanged();

private:
    void rebuildVisible();
    QStringList lines_;
    QStringList visible_lines_;
    QString hex_filter_;
    bool paused_{false};
};

struct SessionEntry { QString name; QString path; QString source; QString startedAt; };
class SessionModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { NameRole = Qt::UserRole + 1, PathRole, SourceRole, StartedAtRole };
    explicit SessionModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSessions(QList<SessionEntry> sessions);
private:
    QList<SessionEntry> sessions_;
};

struct SourceEntry { core::DataSourceType type; QString name; bool available; };
class SourceModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { TypeRole = Qt::UserRole + 1, NameRole, AvailableRole };
    explicit SourceModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSources(QList<SourceEntry> sources);
private:
    QList<SourceEntry> sources_;
};

class SerialPortModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { PortNameRole = Qt::UserRole + 1, FriendlyNameRole, TransportRole, DescriptionRole };
    explicit SerialPortModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setPorts(std::vector<drivers::SerialPortInfo> ports);
private:
    std::vector<drivers::SerialPortInfo> ports_;
};

} // namespace revdash::app
