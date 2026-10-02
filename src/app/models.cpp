#include "app/models.hpp"

#include <QThread>

#include <array>
#include <chrono>

namespace revdash::app {
namespace {
QString text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
constexpr std::array kDashboardMetrics{
    core::MetricId::Rpm,
    core::MetricId::VehicleSpeed,
    core::MetricId::ThrottlePosition,
    core::MetricId::CoolantTemp,
    core::MetricId::EngineLoad,
    core::MetricId::Map,
    core::MetricId::Maf,
    core::MetricId::ShortTermFuelTrim1,
    core::MetricId::LongTermFuelTrim1,
    core::MetricId::ShortTermFuelTrim2,
    core::MetricId::LongTermFuelTrim2,
    core::MetricId::ModuleVoltage,
};
QString metricName(core::MetricId id) {
    switch (id) {
        case core::MetricId::VehicleSpeed: return QObject::tr("Speed");
        case core::MetricId::ThrottlePosition: return QObject::tr("Throttle");
        case core::MetricId::CoolantTemp: return QObject::tr("Coolant");
        case core::MetricId::EngineLoad: return QObject::tr("Engine load");
        case core::MetricId::ShortTermFuelTrim1: return QObject::tr("Short trim B1");
        case core::MetricId::LongTermFuelTrim1: return QObject::tr("Long trim B1");
        case core::MetricId::ShortTermFuelTrim2: return QObject::tr("Short trim B2");
        case core::MetricId::LongTermFuelTrim2: return QObject::tr("Long trim B2");
        case core::MetricId::ModuleVoltage: return QObject::tr("Module voltage");
        default: return text(core::toString(id));
    }
}
QString ecuText(const std::optional<core::EcuAddress>& ecu) {
    return ecu ? QStringLiteral("0x%1").arg(ecu->value, 0, 16).toUpper() : QString{};
}
double displayValue(core::MetricId id, double value, TelemetryModel::UnitSystem system) {
    if (system == TelemetryModel::UnitSystem::Imperial) {
        if (id == core::MetricId::VehicleSpeed) return value * 0.621371192;
        if (id == core::MetricId::CoolantTemp || id == core::MetricId::AmbientAirTemp) return value * 9.0 / 5.0 + 32.0;
        if (id == core::MetricId::Map) return value * 0.145037738;
    }
    return value;
}
QString displayUnit(core::MetricId id, TelemetryModel::UnitSystem system) {
    if (system == TelemetryModel::UnitSystem::Imperial) {
        if (id == core::MetricId::VehicleSpeed) return QStringLiteral("mph");
        if (id == core::MetricId::CoolantTemp || id == core::MetricId::AmbientAirTemp) return QStringLiteral("degF");
        if (id == core::MetricId::Map) return QStringLiteral("psi");
    }
    return text(core::getCanonicalUnit(id));
}
void assertOwnerThread(const QObject* object) { Q_ASSERT(QThread::currentThread() == object->thread()); }
}

TelemetryModel::TelemetryModel(QObject* parent) : QAbstractListModel(parent) {
    for (std::size_t i = 0; i < core::kMetricCount; ++i) snapshot_.samples[i].metric_id = static_cast<core::MetricId>(i);
}
int TelemetryModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(kDashboardMetrics.size()); }
QVariant TelemetryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto id = kDashboardMetrics[static_cast<std::size_t>(index.row())];
    const auto& sample = snapshot_.get(id);
    const auto age = std::max(std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(core::MonotonicClock::now() - sample.monotonic_ts));
    switch (role) {
        case MetricIdRole: return static_cast<int>(id);
        case NameRole: return metricName(id);
        case ValueRole: return displayValue(id, sample.value, unit_system_);
        case UnitRole: return displayUnit(id, unit_system_);
        case QualityRole: return text(core::toString(sample.quality));
        case ValidRole: return sample.isValid();
        case SampleAgeMsRole: return static_cast<qlonglong>(age.count());
        case StateLabelRole:
            if (sample.quality == core::SampleQuality::Unsupported) return tr("Unsupported");
            if (sample.quality == core::SampleQuality::Stale) return tr("Stale");
            if (sample.quality == core::SampleQuality::Dropped) return tr("Dropped");
            if (sample.quality == core::SampleQuality::Invalid) return tr("Invalid");
            return tr("Live");
        default: return {};
    }
}
QHash<int, QByteArray> TelemetryModel::roleNames() const {
    return {{MetricIdRole,"metricId"},{NameRole,"name"},{ValueRole,"value"},{UnitRole,"unit"},{QualityRole,"quality"},{ValidRole,"valid"},
            {SampleAgeMsRole,"sampleAgeMs"},{StateLabelRole,"stateLabel"}};
}
void TelemetryModel::setUnitSystem(UnitSystem value) {
    assertOwnerThread(this);
    if (unit_system_ == value) return;
    unit_system_ = value;
    emit dataChanged(index(0), index(rowCount()-1), {ValueRole, UnitRole});
    emit unitSystemChanged();
}
void TelemetryModel::setSnapshot(const core::TelemetrySnapshot& snapshot) {
    assertOwnerThread(this);
    snapshot_ = snapshot;
    emit dataChanged(index(0), index(rowCount()-1), {ValueRole, QualityRole, ValidRole, SampleAgeMsRole, StateLabelRole});
}

double TelemetryModel::presentationValue(core::MetricId id, double value) const {
    return displayValue(id, value, unit_system_);
}

qint64 TelemetryModel::maximumSampleAgeMs() const {
    const auto now = core::MonotonicClock::now();
    std::chrono::milliseconds maximum{0};
    bool found = false;
    for (const auto id : kDashboardMetrics) {
        const auto& sample = snapshot_.get(id);
        if (sample.quality == core::SampleQuality::Unsupported) continue;
        maximum = std::max(maximum, std::chrono::duration_cast<std::chrono::milliseconds>(now - sample.monotonic_ts));
        found = true;
    }
    return found ? std::max<qint64>(0, maximum.count()) : 0;
}

int DtcModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(records_.size()); }
QVariant DtcModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& item = records_[static_cast<std::size_t>(index.row())];
    switch(role) { case CodeRole:return QString::fromStdString(item.code); case StatusRole:return text(core::toString(item.status)); case SeverityRole:return text(core::toString(item.severity)); case DescriptionRole:return QString::fromStdString(item.description); case EcuRole:return ecuText(item.ecu_address); default:return {}; }
}
QHash<int,QByteArray> DtcModel::roleNames() const { return {{CodeRole,"code"},{StatusRole,"status"},{SeverityRole,"severity"},{DescriptionRole,"description"},{EcuRole,"ecu"}}; }
void DtcModel::setRecords(std::vector<core::DtcRecord> records) { assertOwnerThread(this); beginResetModel(); records_=std::move(records); endResetModel(); }

int FindingModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(findings_.size()); }
QVariant FindingModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row()<0 || index.row()>=rowCount()) return {};
    const auto& item=findings_[static_cast<std::size_t>(index.row())];
    switch(role) { case RuleIdRole:return QString::fromStdString(item.rule_id); case TitleRole:return QString::fromStdString(item.title); case DescriptionRole:return QString::fromStdString(item.description); case SeverityRole:return text(core::toString(item.severity)); case ActiveRole:return item.active; default:return {}; }
}
QHash<int,QByteArray> FindingModel::roleNames() const { return {{RuleIdRole,"ruleId"},{TitleRole,"title"},{DescriptionRole,"description"},{SeverityRole,"severity"},{ActiveRole,"active"}}; }
void FindingModel::setFindings(std::vector<core::DiagnosticFinding> findings) { assertOwnerThread(this); beginResetModel(); findings_=std::move(findings); endResetModel(); }

int SessionModel::rowCount(const QModelIndex& parent) const { return parent.isValid()?0:sessions_.size(); }
QVariant SessionModel::data(const QModelIndex& index,int role) const { if(!index.isValid()||index.row()<0||index.row()>=sessions_.size())return{}; const auto& item=sessions_[index.row()]; switch(role){case NameRole:return item.name;case PathRole:return item.path;case SourceRole:return item.source;case StartedAtRole:return item.startedAt;default:return{};} }
QHash<int,QByteArray> SessionModel::roleNames() const { return {{NameRole,"name"},{PathRole,"path"},{SourceRole,"source"},{StartedAtRole,"startedAt"}}; }
void SessionModel::setSessions(QList<SessionEntry> sessions) { assertOwnerThread(this); beginResetModel(); sessions_=std::move(sessions); endResetModel(); }

SourceModel::SourceModel(QObject* parent):QAbstractListModel(parent),sources_{{core::DataSourceType::SerialElm327,tr("ELM327"),true},{core::DataSourceType::Synthetic,tr("Simulation"),true},{core::DataSourceType::Playback,tr("Session playback"),true},{core::DataSourceType::SocketCan,tr("SocketCAN"),false}} {}
int SourceModel::rowCount(const QModelIndex& parent) const { return parent.isValid()?0:sources_.size(); }
QVariant SourceModel::data(const QModelIndex& index,int role) const { if(!index.isValid()||index.row()<0||index.row()>=sources_.size())return{}; const auto& item=sources_[index.row()]; switch(role){case TypeRole:return static_cast<int>(item.type);case NameRole:return item.name;case AvailableRole:return item.available;default:return{};} }
QHash<int,QByteArray> SourceModel::roleNames() const { return {{TypeRole,"sourceType"},{NameRole,"name"},{AvailableRole,"available"}}; }
void SourceModel::setSources(QList<SourceEntry> sources) { assertOwnerThread(this); beginResetModel(); sources_=std::move(sources); endResetModel(); }

int SerialPortModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(ports_.size()); }
QVariant SerialPortModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& port = ports_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case PortNameRole: return QString::fromStdString(port.port_name);
        case FriendlyNameRole: return QString::fromStdString(port.friendly_name);
        case TransportRole: return port.is_bluetooth_spp ? tr("Bluetooth Classic") : tr("USB / serial");
        case DescriptionRole: return QString::fromStdString(port.device_description);
        default: return {};
    }
}
QHash<int, QByteArray> SerialPortModel::roleNames() const { return {{PortNameRole,"portName"},{FriendlyNameRole,"friendlyName"},{TransportRole,"transport"},{DescriptionRole,"description"}}; }
void SerialPortModel::setPorts(std::vector<drivers::SerialPortInfo> ports) { assertOwnerThread(this); beginResetModel(); ports_ = std::move(ports); endResetModel(); }
} // namespace revdash::app
