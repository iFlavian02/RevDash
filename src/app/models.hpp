#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include "revdash/core/diagnostic_types.hpp"
#include "revdash/core/telemetry_types.hpp"

namespace revdash::app {

class TelemetryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(UnitSystem unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)

public:
    enum class UnitSystem { Metric, Imperial };
    Q_ENUM(UnitSystem)
    enum Role { MetricIdRole = Qt::UserRole + 1, NameRole, ValueRole, UnitRole, QualityRole, ValidRole };

    explicit TelemetryModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    UnitSystem unitSystem() const noexcept { return unit_system_; }
    void setUnitSystem(UnitSystem value);
    void setSnapshot(const core::TelemetrySnapshot& snapshot);

signals:
    void unitSystemChanged();

private:
    core::TelemetrySnapshot snapshot_{};
    UnitSystem unit_system_{UnitSystem::Metric};
};

class DtcModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { CodeRole = Qt::UserRole + 1, StatusRole, SeverityRole, DescriptionRole, EcuRole };
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
    enum Role { RuleIdRole = Qt::UserRole + 1, TitleRole, DescriptionRole, SeverityRole, ActiveRole };
    explicit FindingModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setFindings(std::vector<core::DiagnosticFinding> findings);
private:
    std::vector<core::DiagnosticFinding> findings_;
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

} // namespace revdash::app
