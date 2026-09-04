#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "revdash/core/data_source.hpp"
#include "revdash/core/diagnostic_types.hpp"

namespace revdash::session {

inline constexpr std::uint32_t kSessionSchemaVersion = 1;

enum class SessionRecordType : std::uint8_t {
    Header,
    ObdMessage,
    Telemetry,
    Dtc,
    DiagnosticFinding,
    Mode04Audit,
    EcuMetadata,
    DataLoss,
    Footer
};

[[nodiscard]] constexpr std::string_view toString(SessionRecordType type) noexcept {
    switch (type) {
        case SessionRecordType::Header: return "header";
        case SessionRecordType::ObdMessage: return "obd_message";
        case SessionRecordType::Telemetry: return "telemetry";
        case SessionRecordType::Dtc: return "dtc";
        case SessionRecordType::DiagnosticFinding: return "diagnostic_finding";
        case SessionRecordType::Mode04Audit: return "mode04_audit";
        case SessionRecordType::EcuMetadata: return "ecu_metadata";
        case SessionRecordType::DataLoss: return "data_loss";
        case SessionRecordType::Footer: return "footer";
    }
    return "unknown";
}

struct SimulationMetadata {
    std::uint32_t seed{0};
    double displacement_liters{0.0};
    std::uint32_t cylinder_count{0};
    double initial_rpm{0.0};
    double ambient_temp_c{0.0};
};

struct SessionHeader {
    std::string uuid;
    std::string application_version{std::string{core::kApplicationVersion}};
    std::uint32_t schema_version{kSessionSchemaVersion};
    core::UtcTimePoint utc_start{};
    core::DataSourceType source_type{core::DataSourceType::Synthetic};
    std::map<std::string, std::string> adapter_metadata{};
    std::map<std::string, std::string> protocol_metadata{};
    std::vector<core::EcuMetadata> vehicle_metadata{};
    std::optional<SimulationMetadata> simulation{std::nullopt};
};

struct SessionStatistics {
    std::uint64_t obd_messages{0};
    std::uint64_t telemetry_samples{0};
    std::uint64_t dtcs{0};
    std::uint64_t diagnostic_findings{0};
    std::uint64_t mode04_audits{0};
    std::uint64_t ecu_metadata{0};
    std::uint64_t data_loss_markers{0};
    std::uint64_t dropped_records{0};
    std::uint64_t total_records{0};
    std::uint64_t serialization_buffer_growths{0};
};

// Abstracted to make disk failures deterministic in unit tests. Production uses
// FileSessionStorage, which owns one binary output stream at a time.
class ISessionStorage {
public:
    virtual ~ISessionStorage() = default;
    virtual core::Result<void> open(const std::filesystem::path& partial_path) = 0;
    virtual core::Result<void> write(std::string_view bytes) = 0;
    virtual core::Result<void> flush() = 0;
    virtual core::Result<void> close() = 0;
    virtual core::Result<void> publish(
        const std::filesystem::path& partial_path,
        const std::filesystem::path& final_path) = 0;
};

class FileSessionStorage final : public ISessionStorage {
public:
    FileSessionStorage();
    ~FileSessionStorage() override;
    FileSessionStorage(const FileSessionStorage&) = delete;
    FileSessionStorage& operator=(const FileSessionStorage&) = delete;

    core::Result<void> open(const std::filesystem::path& partial_path) override;
    core::Result<void> write(std::string_view bytes) override;
    core::Result<void> flush() override;
    core::Result<void> close() override;
    core::Result<void> publish(
        const std::filesystem::path& partial_path,
        const std::filesystem::path& final_path) override;

private:
    class State;
    std::unique_ptr<State> state_;
};

class JsonlSessionRecorder final {
public:
    explicit JsonlSessionRecorder(
        std::unique_ptr<ISessionStorage> storage = std::make_unique<FileSessionStorage>());
    ~JsonlSessionRecorder();
    JsonlSessionRecorder(const JsonlSessionRecorder&) = delete;
    JsonlSessionRecorder& operator=(const JsonlSessionRecorder&) = delete;

    core::Result<void> start(
        const std::filesystem::path& output_path,
        SessionHeader header,
        core::MonotonicTimePoint monotonic_start);
    core::Result<void> record(const core::ObdMessage& message);
    core::Result<void> record(const core::TelemetrySample& sample);
    core::Result<void> record(const core::DtcRecord& dtc, core::MonotonicTimePoint timestamp);
    core::Result<void> record(const core::DiagnosticFinding& finding, core::MonotonicTimePoint timestamp);
    core::Result<void> record(const core::Mode04AuditRecord& audit);
    core::Result<void> record(const core::EcuMetadata& metadata, core::MonotonicTimePoint timestamp);
    core::Result<void> recordDataLoss(std::uint64_t dropped_records, core::MonotonicTimePoint timestamp);
    core::Result<void> observeQueueDrops(std::uint64_t cumulative_drops, core::MonotonicTimePoint timestamp);
    core::Result<void> finish(core::MonotonicTimePoint timestamp);

    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] const std::filesystem::path& finalPath() const noexcept { return final_path_; }
    [[nodiscard]] const std::filesystem::path& partialPath() const noexcept { return partial_path_; }
    [[nodiscard]] const SessionStatistics& statistics() const noexcept { return statistics_; }

private:
    core::Result<void> writeRecord(nlohmann::json record, SessionRecordType type);
    [[nodiscard]] std::int64_t elapsedUs(core::MonotonicTimePoint timestamp) const noexcept;

    std::unique_ptr<ISessionStorage> storage_;
    std::filesystem::path final_path_;
    std::filesystem::path partial_path_;
    core::MonotonicTimePoint monotonic_start_{};
    SessionStatistics statistics_{};
    std::uint64_t observed_queue_drops_{0};
    std::string serialization_buffer_;
    bool open_{false};
};

[[nodiscard]] std::filesystem::path partialSessionPath(const std::filesystem::path& final_path);
[[nodiscard]] std::string generateSessionUuid();
[[nodiscard]] core::Result<nlohmann::json> parseSessionRecord(std::string_view line);

} // namespace revdash::session
