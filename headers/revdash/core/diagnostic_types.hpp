#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "revdash/core/telemetry_types.hpp"

namespace revdash::core {

enum class Severity : std::uint8_t {
    Advisory = 0,
    Warning,
    Critical
};

[[nodiscard]] constexpr std::string_view toString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Advisory: return "Advisory";
        case Severity::Warning: return "Warning";
        case Severity::Critical: return "Critical";
    }
    return "Unknown";
}

enum class DtcStatus : std::uint8_t {
    Confirmed = 0,
    Pending,
    Permanent
};

[[nodiscard]] constexpr std::string_view toString(DtcStatus status) noexcept {
    switch (status) {
        case DtcStatus::Confirmed: return "Confirmed";
        case DtcStatus::Pending: return "Pending";
        case DtcStatus::Permanent: return "Permanent";
    }
    return "Unknown";
}

struct FreezeFrame {
    std::string dtc_code;
    std::uint8_t frame_number{0};
    MonotonicTimePoint timestamp{MonotonicClock::now()};
    std::vector<TelemetrySample> samples{};
};

struct DtcRecord {
    std::string code;
    DtcStatus status{DtcStatus::Confirmed};
    Severity severity{Severity::Warning};
    std::string description;
    std::vector<std::string> likely_failure_points{};
    std::optional<EcuAddress> ecu_address{std::nullopt};
    std::optional<FreezeFrame> freeze_frame{std::nullopt};

    [[nodiscard]] bool operator==(const DtcRecord& other) const noexcept {
        return code == other.code && status == other.status && ecu_address == other.ecu_address;
    }
};

struct EcuMetadata {
    std::optional<EcuAddress> ecu_address{std::nullopt};
    std::string vin;
    std::vector<std::string> calibration_ids{};
    std::vector<std::string> cvns{};
    std::string protocol_name;
};

struct DiagnosticFinding {
    std::string rule_id;
    std::string rule_version{"1.0"};
    Severity severity{Severity::Warning};
    std::string title;
    std::string description;
    std::vector<std::string> evidence{};
    MonotonicTimePoint first_detected{MonotonicClock::now()};
    MonotonicTimePoint last_seen{MonotonicClock::now()};
    MonotonicTimePoint last_evaluated{MonotonicClock::now()};
    std::optional<MonotonicTimePoint> resolved_at{std::nullopt};
    bool active{true};
};

struct DiagnosticSnapshot {
    std::vector<DtcRecord> dtcs{};
    std::vector<EcuMetadata> ecu_metadata{};
    std::vector<DiagnosticFinding> findings{};
    MonotonicTimePoint captured_at{MonotonicClock::now()};
    std::uint64_t engine_epoch{0};
};

inline constexpr std::string_view kClearDiagnosticWarning =
    "Clearing emissions-related diagnostic information may erase freeze-frame data and reset readiness-related state.";

struct ClearDtcPreparation {
    std::string confirmation_token;
    MonotonicTimePoint expires_at{MonotonicClock::now()};
    DiagnosticSnapshot snapshot{};
    std::optional<std::string> vehicle_identity{std::nullopt};
    std::string warning{std::string{kClearDiagnosticWarning}};
};

struct Mode04AuditRecord {
    MonotonicTimePoint prepared_at{MonotonicClock::now()};
    MonotonicTimePoint completed_at{MonotonicClock::now()};
    std::optional<UtcTimePoint> completed_utc{std::nullopt};
    DataSourceType source_type{DataSourceType::SerialElm327};
    std::uint64_t engine_epoch{0};
    std::optional<std::string> vehicle_identity{std::nullopt};
    std::string warning{std::string{kClearDiagnosticWarning}};
    DiagnosticSnapshot preparation_snapshot{};
    std::vector<DtcRecord> post_clear_dtcs{};
    bool request_transmitted{false};
    bool positive_response{false};
    bool post_clear_rescan_completed{false};
    std::optional<Error> error{std::nullopt};
};

} // namespace revdash::core
