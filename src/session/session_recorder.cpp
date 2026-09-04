#include "revdash/session/session_recorder.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

namespace revdash::session {
namespace {

using Json = nlohmann::json;

[[nodiscard]] core::Result<void> storageError(std::string message, std::string context = {}) {
    return core::makeError(core::ErrorCode::StorageUnavailable, std::move(message), false, std::move(context));
}

[[nodiscard]] core::Result<void> sessionError(std::string message, std::string context = {}) {
    return core::makeError(core::ErrorCode::SessionInvalidFormat, std::move(message), false, std::move(context));
}

[[nodiscard]] std::string formatUtc(core::UtcTimePoint time) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    auto fractional = (milliseconds - seconds).count();
    auto whole_seconds = core::SystemClock::to_time_t(core::UtcTimePoint{seconds});
    if (fractional < 0) {
        fractional += 1000;
        --whole_seconds;
    }
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &whole_seconds);
#else
    gmtime_r(&whole_seconds, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << fractional << 'Z';
    return output.str();
}

[[nodiscard]] std::string addressFormat(core::EcuAddressFormat format) {
    switch (format) {
        case core::EcuAddressFormat::Can11Bit: return "can_11_bit";
        case core::EcuAddressFormat::Can29Bit: return "can_29_bit";
        case core::EcuAddressFormat::Other: return "other";
    }
    return "other";
}

[[nodiscard]] Json addressJson(const std::optional<core::EcuAddress>& address) {
    if (!address) return nullptr;
    return Json{{"format", addressFormat(address->format)}, {"value", address->value}};
}

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string output(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output[index * 2U] = digits[bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return output;
}

[[nodiscard]] std::int64_t relativeUs(core::MonotonicTimePoint value, core::MonotonicTimePoint origin) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(value - origin).count();
}

[[nodiscard]] Json sampleJson(const core::TelemetrySample& sample, core::MonotonicTimePoint origin) {
    Json result{{"metric", core::toString(sample.metric_id)},
                {"value", sample.value},
                {"unit", core::getCanonicalUnit(sample.metric_id)},
                {"quality", core::toString(sample.quality)},
                {"sample_elapsed_us", relativeUs(sample.monotonic_ts, origin)},
                {"sequence", sample.sequence_number},
                {"ecu", addressJson(sample.ecu_address)}};
    if (sample.utc_ts) result["utc"] = formatUtc(*sample.utc_ts);
    return result;
}

[[nodiscard]] Json freezeFrameJson(const core::FreezeFrame& frame, core::MonotonicTimePoint origin) {
    Json samples = Json::array();
    for (const auto& sample : frame.samples) samples.push_back(sampleJson(sample, origin));
    return Json{{"dtc_code", frame.dtc_code},
                {"frame_number", frame.frame_number},
                {"captured_elapsed_us", relativeUs(frame.timestamp, origin)},
                {"samples", std::move(samples)}};
}

[[nodiscard]] Json dtcJson(const core::DtcRecord& dtc, core::MonotonicTimePoint origin) {
    Json result{{"code", dtc.code},
                {"status", core::toString(dtc.status)},
                {"severity", core::toString(dtc.severity)},
                {"description", dtc.description},
                {"likely_failure_points", dtc.likely_failure_points},
                {"ecu", addressJson(dtc.ecu_address)}};
    result["freeze_frame"] = dtc.freeze_frame ? freezeFrameJson(*dtc.freeze_frame, origin) : Json{nullptr};
    return result;
}

[[nodiscard]] Json findingJson(const core::DiagnosticFinding& finding, core::MonotonicTimePoint origin) {
    Json result{{"rule_id", finding.rule_id},
                {"rule_version", finding.rule_version},
                {"severity", core::toString(finding.severity)},
                {"title", finding.title},
                {"description", finding.description},
                {"evidence", finding.evidence},
                {"first_detected_elapsed_us", relativeUs(finding.first_detected, origin)},
                {"last_seen_elapsed_us", relativeUs(finding.last_seen, origin)},
                {"last_evaluated_elapsed_us", relativeUs(finding.last_evaluated, origin)},
                {"active", finding.active}};
    result["resolved_elapsed_us"] = finding.resolved_at
        ? Json{relativeUs(*finding.resolved_at, origin)} : Json{nullptr};
    return result;
}

[[nodiscard]] Json metadataJson(const core::EcuMetadata& metadata) {
    return Json{{"ecu", addressJson(metadata.ecu_address)},
                {"vin", metadata.vin},
                {"calibration_ids", metadata.calibration_ids},
                {"cvns", metadata.cvns},
                {"protocol_name", metadata.protocol_name}};
}

[[nodiscard]] Json snapshotJson(const core::DiagnosticSnapshot& snapshot, core::MonotonicTimePoint origin) {
    Json dtcs = Json::array();
    for (const auto& dtc : snapshot.dtcs) dtcs.push_back(dtcJson(dtc, origin));
    Json metadata = Json::array();
    for (const auto& ecu : snapshot.ecu_metadata) metadata.push_back(metadataJson(ecu));
    Json findings = Json::array();
    for (const auto& finding : snapshot.findings) findings.push_back(findingJson(finding, origin));
    return Json{{"dtcs", std::move(dtcs)},
                {"ecu_metadata", std::move(metadata)},
                {"findings", std::move(findings)},
                {"captured_elapsed_us", relativeUs(snapshot.captured_at, origin)},
                {"engine_epoch", snapshot.engine_epoch}};
}

[[nodiscard]] Json errorJson(const std::optional<core::Error>& error) {
    if (!error) return nullptr;
    return Json{{"domain", core::toString(error->domain)},
                {"code", error->code},
                {"message", error->message},
                {"retryable", error->retryable},
                {"context", error->context}};
}

[[nodiscard]] Json auditJson(const core::Mode04AuditRecord& audit, core::MonotonicTimePoint origin) {
    Json post_clear_dtcs = Json::array();
    for (const auto& dtc : audit.post_clear_dtcs) post_clear_dtcs.push_back(dtcJson(dtc, origin));
    Json result{{"prepared_elapsed_us", relativeUs(audit.prepared_at, origin)},
                {"completed_elapsed_us", relativeUs(audit.completed_at, origin)},
                {"source_type", core::toString(audit.source_type)},
                {"engine_epoch", audit.engine_epoch},
                {"vehicle_identity", audit.vehicle_identity ? Json{*audit.vehicle_identity} : Json{nullptr}},
                {"warning", audit.warning},
                {"preparation_snapshot", snapshotJson(audit.preparation_snapshot, origin)},
                {"post_clear_dtcs", std::move(post_clear_dtcs)},
                {"request_transmitted", audit.request_transmitted},
                {"positive_response", audit.positive_response},
                {"post_clear_rescan_completed", audit.post_clear_rescan_completed},
                {"error", errorJson(audit.error)}};
    if (audit.completed_utc) result["completed_utc"] = formatUtc(*audit.completed_utc);
    return result;
}

[[nodiscard]] bool isKnownType(std::string_view type) noexcept {
    constexpr std::array types{"header", "obd_message", "telemetry", "dtc", "diagnostic_finding",
                               "mode04_audit", "ecu_metadata", "data_loss", "footer"};
    for (const auto* known : types) if (type == known) return true;
    return false;
}

} // namespace

class FileSessionStorage::State {
public:
    std::ofstream stream;
};

FileSessionStorage::FileSessionStorage() : state_(std::make_unique<State>()) {}
FileSessionStorage::~FileSessionStorage() = default;

core::Result<void> FileSessionStorage::open(const std::filesystem::path& partial_path) {
    std::error_code error;
    const auto parent = partial_path.has_parent_path() ? partial_path.parent_path() : std::filesystem::current_path(error);
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        return storageError("Session output directory is unavailable", partial_path.string());
    }
    if (std::filesystem::exists(partial_path, error) || error) {
        return storageError("A partial session already exists and was left untouched", partial_path.string());
    }
    state_->stream.open(partial_path, std::ios::binary | std::ios::out);
    if (!state_->stream.is_open()) return storageError("Could not create partial session file", partial_path.string());
    return core::makeSuccess();
}

core::Result<void> FileSessionStorage::write(std::string_view bytes) {
    state_->stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!state_->stream) return storageError("Could not write session data");
    return core::makeSuccess();
}

core::Result<void> FileSessionStorage::flush() {
    state_->stream.flush();
    if (!state_->stream) return storageError("Could not flush session data");
    return core::makeSuccess();
}

core::Result<void> FileSessionStorage::close() {
    if (!state_->stream.is_open()) return core::makeSuccess();
    state_->stream.close();
    if (state_->stream.fail()) return storageError("Could not close session data");
    return core::makeSuccess();
}

core::Result<void> FileSessionStorage::publish(
    const std::filesystem::path& partial_path,
    const std::filesystem::path& final_path) {
    std::error_code error;
    if (std::filesystem::exists(final_path, error) || error) {
        return storageError("Final session path already exists", final_path.string());
    }
    std::filesystem::rename(partial_path, final_path, error);
    if (error) return storageError("Could not atomically publish completed session", error.message());
    return core::makeSuccess();
}

JsonlSessionRecorder::JsonlSessionRecorder(std::unique_ptr<ISessionStorage> storage)
    : storage_(storage ? std::move(storage) : std::make_unique<FileSessionStorage>()) {
    serialization_buffer_.reserve(16U * 1024U);
}

JsonlSessionRecorder::~JsonlSessionRecorder() {
    if (open_) static_cast<void>(storage_->close());
}

core::Result<void> JsonlSessionRecorder::start(
    const std::filesystem::path& output_path,
    SessionHeader header,
    core::MonotonicTimePoint monotonic_start) {
    if (open_) return sessionError("A session recording is already active");
    if (output_path.empty() || output_path.extension() != ".jsonl") {
        return sessionError("Session output path must use the .jsonl extension", output_path.string());
    }
    if (header.schema_version != kSessionSchemaVersion || header.uuid.empty()) {
        return sessionError("Session header requires Schema v1 and a non-empty UUID");
    }
    final_path_ = output_path;
    partial_path_ = partialSessionPath(output_path);
    if (auto result = storage_->open(partial_path_); !result) return result;
    monotonic_start_ = monotonic_start;
    statistics_ = {};
    observed_queue_drops_ = 0;
    open_ = true;

    Json vehicle = Json::array();
    for (const auto& metadata : header.vehicle_metadata) vehicle.push_back(metadataJson(metadata));
    Json body{{"uuid", header.uuid},
              {"application_version", header.application_version},
              {"utc_start", formatUtc(header.utc_start)},
              {"source_type", core::toString(header.source_type)},
              {"adapter_metadata", header.adapter_metadata},
              {"protocol_metadata", header.protocol_metadata},
              {"vehicle_metadata", std::move(vehicle)}};
    if (header.simulation) {
        body["simulation"] = Json{{"seed", header.simulation->seed},
                                   {"displacement_liters", header.simulation->displacement_liters},
                                   {"cylinder_count", header.simulation->cylinder_count},
                                   {"initial_rpm", header.simulation->initial_rpm},
                                   {"ambient_temp_c", header.simulation->ambient_temp_c}};
    } else {
        body["simulation"] = nullptr;
    }
    return writeRecord(std::move(body), SessionRecordType::Header);
}

core::Result<void> JsonlSessionRecorder::record(const core::ObdMessage& message) {
    Json body{{"elapsed_us", elapsedUs(message.monotonic_ts)},
              {"source_type", core::toString(message.source_type)},
              {"sequence", message.sequence_number},
              {"ecu", addressJson(message.ecu_address)},
              {"payload_hex", hex(message.payload())}};
    if (message.utc_ts) body["utc"] = formatUtc(*message.utc_ts);
    return writeRecord(std::move(body), SessionRecordType::ObdMessage);
}

core::Result<void> JsonlSessionRecorder::record(const core::TelemetrySample& sample) {
    auto body = sampleJson(sample, monotonic_start_);
    body["elapsed_us"] = elapsedUs(sample.monotonic_ts);
    return writeRecord(std::move(body), SessionRecordType::Telemetry);
}

core::Result<void> JsonlSessionRecorder::record(const core::DtcRecord& dtc, core::MonotonicTimePoint timestamp) {
    auto body = dtcJson(dtc, monotonic_start_);
    body["elapsed_us"] = elapsedUs(timestamp);
    return writeRecord(std::move(body), SessionRecordType::Dtc);
}

core::Result<void> JsonlSessionRecorder::record(
    const core::DiagnosticFinding& finding,
    core::MonotonicTimePoint timestamp) {
    auto body = findingJson(finding, monotonic_start_);
    body["elapsed_us"] = elapsedUs(timestamp);
    return writeRecord(std::move(body), SessionRecordType::DiagnosticFinding);
}

core::Result<void> JsonlSessionRecorder::record(const core::Mode04AuditRecord& audit) {
    auto body = auditJson(audit, monotonic_start_);
    body["elapsed_us"] = elapsedUs(audit.completed_at);
    return writeRecord(std::move(body), SessionRecordType::Mode04Audit);
}

core::Result<void> JsonlSessionRecorder::record(
    const core::EcuMetadata& metadata,
    core::MonotonicTimePoint timestamp) {
    auto body = metadataJson(metadata);
    body["elapsed_us"] = elapsedUs(timestamp);
    return writeRecord(std::move(body), SessionRecordType::EcuMetadata);
}

core::Result<void> JsonlSessionRecorder::recordDataLoss(
    std::uint64_t dropped_records,
    core::MonotonicTimePoint timestamp) {
    if (dropped_records == 0) return core::makeSuccess();
    return writeRecord(Json{{"elapsed_us", elapsedUs(timestamp)}, {"dropped_records", dropped_records}},
                       SessionRecordType::DataLoss);
}

core::Result<void> JsonlSessionRecorder::observeQueueDrops(
    std::uint64_t cumulative_drops,
    core::MonotonicTimePoint timestamp) {
    if (cumulative_drops <= observed_queue_drops_) return core::makeSuccess();
    const auto delta = cumulative_drops - observed_queue_drops_;
    observed_queue_drops_ = cumulative_drops;
    return recordDataLoss(delta, timestamp);
}

core::Result<void> JsonlSessionRecorder::finish(core::MonotonicTimePoint timestamp) {
    if (!open_) return sessionError("No session recording is active");
    Json body{{"elapsed_us", elapsedUs(timestamp)},
              {"statistics", {{"obd_messages", statistics_.obd_messages},
                               {"telemetry_samples", statistics_.telemetry_samples},
                               {"dtcs", statistics_.dtcs},
                               {"diagnostic_findings", statistics_.diagnostic_findings},
                               {"mode04_audits", statistics_.mode04_audits},
                               {"ecu_metadata", statistics_.ecu_metadata},
                               {"data_loss_markers", statistics_.data_loss_markers},
                               {"dropped_records", statistics_.dropped_records},
                               {"serialization_buffer_growths", statistics_.serialization_buffer_growths},
                               {"total_records", statistics_.total_records + 1U}}}};
    if (auto result = writeRecord(std::move(body), SessionRecordType::Footer); !result) return result;
    if (auto result = storage_->flush(); !result) { open_ = false; static_cast<void>(storage_->close()); return result; }
    if (auto result = storage_->close(); !result) { open_ = false; return result; }
    open_ = false;
    return storage_->publish(partial_path_, final_path_);
}

core::Result<void> JsonlSessionRecorder::writeRecord(Json record, SessionRecordType type) {
    if (!open_) return sessionError("No session recording is active");
    record["schema_version"] = kSessionSchemaVersion;
    record["type"] = toString(type);
    if (type == SessionRecordType::Header) record["elapsed_us"] = 0;
    if (!record.contains("elapsed_us") || !record.at("elapsed_us").is_number_integer() ||
        record.at("elapsed_us").get<std::int64_t>() < 0) {
        return sessionError("Session record timestamp precedes the recording start");
    }
    serialization_buffer_.clear();
    const auto previous_capacity = serialization_buffer_.capacity();
    const auto serialized = record.dump();
    serialization_buffer_.append(serialized);
    serialization_buffer_.push_back('\n');
    if (serialization_buffer_.capacity() != previous_capacity) ++statistics_.serialization_buffer_growths;
    if (auto result = storage_->write(serialization_buffer_); !result) {
        open_ = false;
        static_cast<void>(storage_->close());
        return result;
    }
    ++statistics_.total_records;
    switch (type) {
        case SessionRecordType::ObdMessage: ++statistics_.obd_messages; break;
        case SessionRecordType::Telemetry: ++statistics_.telemetry_samples; break;
        case SessionRecordType::Dtc: ++statistics_.dtcs; break;
        case SessionRecordType::DiagnosticFinding: ++statistics_.diagnostic_findings; break;
        case SessionRecordType::Mode04Audit: ++statistics_.mode04_audits; break;
        case SessionRecordType::EcuMetadata: ++statistics_.ecu_metadata; break;
        case SessionRecordType::DataLoss:
            ++statistics_.data_loss_markers;
            statistics_.dropped_records += record.at("dropped_records").get<std::uint64_t>();
            break;
        case SessionRecordType::Header:
        case SessionRecordType::Footer: break;
    }
    return core::makeSuccess();
}

std::int64_t JsonlSessionRecorder::elapsedUs(core::MonotonicTimePoint timestamp) const noexcept {
    return relativeUs(timestamp, monotonic_start_);
}

std::filesystem::path partialSessionPath(const std::filesystem::path& final_path) {
    auto result = final_path;
    result.replace_extension(".partial");
    return result;
}

std::string generateSessionUuid() {
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    const auto encoded = hex(bytes);
    return encoded.substr(0, 8) + '-' + encoded.substr(8, 4) + '-' + encoded.substr(12, 4) + '-' +
           encoded.substr(16, 4) + '-' + encoded.substr(20, 12);
}

core::Result<Json> parseSessionRecord(std::string_view line) {
    try {
        auto record = Json::parse(line);
        if (!record.is_object() || !record.contains("schema_version") ||
            record.at("schema_version") != kSessionSchemaVersion || !record.contains("type") ||
            !record.at("type").is_string() || !isKnownType(record.at("type").get_ref<const std::string&>()) ||
            !record.contains("elapsed_us") || !record.at("elapsed_us").is_number_integer() ||
            record.at("elapsed_us").get<std::int64_t>() < 0) {
            return core::makeError(core::ErrorCode::SessionInvalidFormat,
                                   "Session record is missing required Schema v1 fields");
        }
        return record;
    } catch (const Json::exception& exception) {
        return core::makeError(core::ErrorCode::SessionInvalidFormat,
                               "Session record is not valid JSON", false, exception.what());
    }
}

} // namespace revdash::session
