#include "revdash/drivers/playback.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "revdash/session/session_recorder.hpp"

namespace revdash::drivers {
namespace {

using Json = nlohmann::json;
using Microseconds = std::chrono::microseconds;

struct Checkpoint {
    std::int64_t elapsed_us{0};
    std::uint64_t offset{0};
};

struct PendingMessage {
    std::int64_t elapsed_us{0};
    core::ObdMessage message{};
};

[[nodiscard]] core::Error playbackError(std::string message, std::string context = {}) {
    return core::Error{
        .domain = core::ErrorDomain::Session,
        .code = std::string{core::toString(core::ErrorCode::SessionInvalidFormat)},
        .message = std::move(message),
        .retryable = false,
        .context = std::move(context)};
}

[[nodiscard]] core::Result<void> failure(std::string message, std::string context = {}) {
    return tl::make_unexpected(playbackError(std::move(message), std::move(context)));
}

[[nodiscard]] bool isUnsigned(const Json& value) noexcept {
    return value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

[[nodiscard]] bool hasFields(const Json& value, std::initializer_list<const char*> fields) {
    return std::all_of(fields.begin(), fields.end(), [&](const char* field) { return value.contains(field); });
}

[[nodiscard]] bool stringArray(const Json& value) {
    return value.is_array() && std::all_of(value.begin(), value.end(), [](const Json& item) { return item.is_string(); });
}

[[nodiscard]] bool validSourceType(const Json& value) {
    if (!value.is_string()) return false;
    const auto& source = value.get_ref<const std::string&>();
    return source == "SerialElm327" || source == "Synthetic" || source == "Playback" || source == "SocketCan";
}

[[nodiscard]] bool validAddress(const Json& value) {
    if (value.is_null()) return true;
    if (!value.is_object() || !hasFields(value, {"format", "value"}) ||
        !value.at("format").is_string() || !isUnsigned(value.at("value"))) return false;
    const auto& format = value.at("format").get_ref<const std::string&>();
    return format == "can_11_bit" || format == "can_29_bit" || format == "other";
}

[[nodiscard]] bool validUpperHex(const std::string& value) noexcept {
    if ((value.size() % 2U) != 0U || value.size() > core::kMaxIsoTpPayloadBytes * 2U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F');
    });
}

[[nodiscard]] bool validRecordStructure(const Json& record) {
    const auto& type = record.at("type").get_ref<const std::string&>();
    if (type == "header") {
        return hasFields(record, {"uuid", "application_version", "utc_start", "source_type",
                                  "adapter_metadata", "protocol_metadata", "vehicle_metadata", "simulation"}) &&
               record.at("uuid").is_string() && !record.at("uuid").get_ref<const std::string&>().empty() &&
               record.at("application_version").is_string() && record.at("utc_start").is_string() &&
               validSourceType(record.at("source_type")) && record.at("adapter_metadata").is_object() &&
               record.at("protocol_metadata").is_object() && record.at("vehicle_metadata").is_array() &&
               (record.at("simulation").is_null() || record.at("simulation").is_object());
    }
    if (type == "obd_message") {
        return hasFields(record, {"source_type", "sequence", "ecu", "payload_hex"}) &&
               validSourceType(record.at("source_type")) && isUnsigned(record.at("sequence")) &&
               validAddress(record.at("ecu")) && record.at("payload_hex").is_string() &&
               !record.at("payload_hex").get_ref<const std::string&>().empty() &&
               validUpperHex(record.at("payload_hex").get_ref<const std::string&>());
    }
    if (type == "telemetry") {
        return hasFields(record, {"metric", "value", "unit", "quality", "sample_elapsed_us", "sequence", "ecu"}) &&
               record.at("metric").is_string() && record.at("value").is_number() &&
               record.at("unit").is_string() && record.at("quality").is_string() &&
               record.at("sample_elapsed_us").is_number_integer() && isUnsigned(record.at("sequence")) &&
               validAddress(record.at("ecu"));
    }
    if (type == "dtc") {
        return hasFields(record, {"code", "status", "severity", "description", "likely_failure_points", "ecu", "freeze_frame"}) &&
               record.at("code").is_string() && record.at("status").is_string() &&
               record.at("severity").is_string() && record.at("description").is_string() &&
               stringArray(record.at("likely_failure_points")) && validAddress(record.at("ecu")) &&
               (record.at("freeze_frame").is_null() || record.at("freeze_frame").is_object());
    }
    if (type == "diagnostic_finding") {
        return hasFields(record, {"rule_id", "rule_version", "severity", "title", "description", "evidence",
                                  "first_detected_elapsed_us", "last_seen_elapsed_us", "last_evaluated_elapsed_us",
                                  "resolved_elapsed_us", "active"}) &&
               record.at("rule_id").is_string() && record.at("rule_version").is_string() &&
               record.at("severity").is_string() && record.at("title").is_string() &&
               record.at("description").is_string() && stringArray(record.at("evidence")) &&
               record.at("first_detected_elapsed_us").is_number_integer() &&
               record.at("last_seen_elapsed_us").is_number_integer() &&
               record.at("last_evaluated_elapsed_us").is_number_integer() &&
               (record.at("resolved_elapsed_us").is_null() || record.at("resolved_elapsed_us").is_number_integer()) &&
               record.at("active").is_boolean();
    }
    if (type == "mode04_audit") {
        return hasFields(record, {"prepared_elapsed_us", "completed_elapsed_us", "source_type", "engine_epoch",
                                  "vehicle_identity", "warning", "preparation_snapshot", "post_clear_dtcs",
                                  "request_transmitted", "positive_response", "post_clear_rescan_completed", "error"}) &&
               record.at("prepared_elapsed_us").is_number_integer() && record.at("completed_elapsed_us").is_number_integer() &&
               validSourceType(record.at("source_type")) && isUnsigned(record.at("engine_epoch")) &&
               (record.at("vehicle_identity").is_null() || record.at("vehicle_identity").is_string()) &&
               record.at("warning").is_string() && record.at("preparation_snapshot").is_object() &&
               record.at("post_clear_dtcs").is_array() && record.at("request_transmitted").is_boolean() &&
               record.at("positive_response").is_boolean() && record.at("post_clear_rescan_completed").is_boolean() &&
               (record.at("error").is_null() || record.at("error").is_object());
    }
    if (type == "ecu_metadata") {
        return hasFields(record, {"ecu", "vin", "calibration_ids", "cvns", "protocol_name"}) &&
               validAddress(record.at("ecu")) && record.at("vin").is_string() &&
               stringArray(record.at("calibration_ids")) && stringArray(record.at("cvns")) &&
               record.at("protocol_name").is_string();
    }
    if (type == "data_loss") return record.contains("dropped_records") && isUnsigned(record.at("dropped_records")) && record.at("dropped_records").get<std::uint64_t>() > 0;
    if (type == "footer") {
        if (!record.contains("statistics") || !record.at("statistics").is_object()) return false;
        const auto& statistics = record.at("statistics");
        constexpr std::array fields{"obd_messages", "telemetry_samples", "dtcs", "diagnostic_findings",
                                    "mode04_audits", "ecu_metadata", "data_loss_markers", "dropped_records",
                                    "serialization_buffer_growths", "total_records"};
        return std::all_of(fields.begin(), fields.end(), [&](const char* field) {
            return statistics.contains(field) && isUnsigned(statistics.at(field));
        });
    }
    return false;
}

[[nodiscard]] std::optional<core::EcuAddress> parseAddress(const Json& value) {
    if (value.is_null()) return std::nullopt;
    const auto& format = value.at("format").get_ref<const std::string&>();
    const auto parsed_format = format == "can_11_bit" ? core::EcuAddressFormat::Can11Bit
        : format == "can_29_bit" ? core::EcuAddressFormat::Can29Bit : core::EcuAddressFormat::Other;
    return core::EcuAddress{value.at("value").get<std::uint32_t>(), parsed_format};
}

[[nodiscard]] std::vector<std::uint8_t> decodeHex(const std::string& encoded) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(encoded.size() / 2U);
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        unsigned int value = 0;
        const auto* begin = encoded.data() + index;
        const auto result = std::from_chars(begin, begin + 2, value, 16);
        if (result.ec != std::errc{}) return {};
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

[[nodiscard]] core::Result<PendingMessage> parseMessage(const Json& record, core::MonotonicTimePoint timestamp) {
    const auto bytes = decodeHex(record.at("payload_hex").get_ref<const std::string&>());
    auto message = core::ObdMessage::create(
        core::DataSourceType::Playback,
        parseAddress(record.at("ecu")),
        bytes,
        record.at("sequence").get<std::uint64_t>(),
        timestamp,
        std::nullopt);
    if (!message) return tl::make_unexpected(message.error());
    return PendingMessage{.elapsed_us = record.at("elapsed_us").get<std::int64_t>(), .message = *message};
}

[[nodiscard]] std::filesystem::path indexPath(const std::filesystem::path& session_path) {
    auto result = session_path;
    result.replace_extension(".ridx");
    return result;
}

} // namespace

struct PlaybackDataSource::Impl {
    explicit Impl(std::shared_ptr<core::IClock> playback_clock) : clock(std::move(playback_clock)) {}

    std::shared_ptr<core::IClock> clock;
    std::filesystem::path path;
    std::ifstream stream;
    std::vector<Checkpoint> checkpoints;
    mutable std::mutex historical_mutex;
    std::vector<session::HistoricalSessionRecord> findings;
    std::vector<session::HistoricalSessionRecord> audits;
    std::optional<PendingMessage> next;
    core::MonotonicTimePoint session_origin{};
    core::MonotonicTimePoint play_anchor{};
    std::int64_t play_anchor_us{0};
    std::atomic<std::int64_t> position_us{0};
    std::atomic<std::int64_t> duration_us{0};
    std::atomic<PlaybackState> state{PlaybackState::Stopped};
    std::atomic<bool> index_rebuilt{false};
    double speed{1.0};
    bool loop{false};
    std::uint64_t generation{0};

    [[nodiscard]] std::int64_t currentVirtualPosition() const noexcept {
        if (state.load(std::memory_order_acquire) != PlaybackState::Playing) {
            return position_us.load(std::memory_order_acquire);
        }
        const auto wall_elapsed = std::chrono::duration_cast<Microseconds>(clock->monotonicNow() - play_anchor).count();
        const auto virtual_elapsed = play_anchor_us + static_cast<std::int64_t>(static_cast<double>(wall_elapsed) * speed);
        return std::clamp<std::int64_t>(virtual_elapsed, 0, duration_us.load(std::memory_order_acquire));
    }

    core::Result<void> validateAndIndex() {
        std::ifstream input(path, std::ios::binary);
        if (!input) return failure("Playback session could not be opened", path.string());

        std::uint64_t hash = 14695981039346656037ULL;
        std::vector<Checkpoint> rebuilt;
        std::vector<session::HistoricalSessionRecord> rebuilt_findings;
        std::vector<session::HistoricalSessionRecord> rebuilt_audits;
        std::string line;
        std::int64_t previous_elapsed = -1;
        std::int64_t next_checkpoint = 0;
        std::uint64_t line_number = 0;
        bool saw_header = false;
        bool saw_footer = false;
        while (true) {
            const auto offset = static_cast<std::uint64_t>(input.tellg());
            if (!std::getline(input, line)) break;
            ++line_number;
            for (const unsigned char character : line) { hash ^= character; hash *= 1099511628211ULL; }
            hash ^= static_cast<unsigned char>('\n'); hash *= 1099511628211ULL;
            const auto parsed = session::parseSessionRecord(line);
            if (!parsed) return failure(parsed.error().message, "line " + std::to_string(line_number));
            const auto& record = *parsed;
            if (!validRecordStructure(record)) return failure("Session record has invalid or missing fields", "line " + std::to_string(line_number));
            const auto elapsed = record.at("elapsed_us").get<std::int64_t>();
            if (elapsed < previous_elapsed) return failure("Session elapsed timestamps are not monotonic", "line " + std::to_string(line_number));
            previous_elapsed = elapsed;
            const auto& type = record.at("type").get_ref<const std::string&>();
            if (line_number == 1 && type != "header") return failure("Session header must be the first record");
            if (type == "header") {
                if (saw_header || line_number != 1 || elapsed != 0) return failure("Session contains an invalid header position");
                saw_header = true;
            } else if (type == "footer") {
                if (saw_footer) return failure("Session contains more than one footer");
                saw_footer = true;
            } else if (saw_footer) {
                return failure("Session footer must be the final record", "line " + std::to_string(line_number));
            }
            if (elapsed >= next_checkpoint) {
                rebuilt.push_back(Checkpoint{.elapsed_us = elapsed, .offset = offset});
                next_checkpoint = ((elapsed / 1'000'000) + 1) * 1'000'000;
            }
            if (type == "diagnostic_finding") rebuilt_findings.push_back({.elapsed_us = elapsed, .value = record});
            if (type == "mode04_audit") rebuilt_audits.push_back({.elapsed_us = elapsed, .value = record});
        }
        if (!saw_header || !saw_footer) return failure("Session is truncated because its header or footer is missing");

        const auto size = std::filesystem::file_size(path);
        bool valid_existing_index = false;
        std::ifstream existing(indexPath(path), std::ios::binary);
        if (existing) {
            try {
                const auto json = Json::parse(existing);
                valid_existing_index = json.at("index_version") == 1 &&
                    json.at("schema_version") == session::kSessionSchemaVersion &&
                    json.at("source_size").get<std::uint64_t>() == size &&
                    json.at("source_hash").get<std::uint64_t>() == hash &&
                    json.at("duration_us").get<std::int64_t>() == previous_elapsed &&
                    json.at("checkpoints").is_array() &&
                    json.at("checkpoints").size() == rebuilt.size();
                if (valid_existing_index) {
                    for (std::size_t index = 0; index < rebuilt.size(); ++index) {
                        const auto& stored = json.at("checkpoints").at(index);
                        valid_existing_index = stored.is_object() &&
                            stored.at("elapsed_us").get<std::int64_t>() == rebuilt[index].elapsed_us &&
                            stored.at("offset").get<std::uint64_t>() == rebuilt[index].offset;
                        if (!valid_existing_index) break;
                    }
                }
            } catch (const Json::exception&) {
                valid_existing_index = false;
            }
        }
        index_rebuilt.store(!valid_existing_index, std::memory_order_release);
        if (!valid_existing_index) {
            Json checkpoint_json = Json::array();
            for (const auto& checkpoint : rebuilt) checkpoint_json.push_back({{"elapsed_us", checkpoint.elapsed_us}, {"offset", checkpoint.offset}});
            const Json index{{"index_version", 1},
                             {"schema_version", session::kSessionSchemaVersion},
                             {"source_size", size},
                             {"source_hash", hash},
                             {"duration_us", previous_elapsed},
                             {"checkpoints", std::move(checkpoint_json)}};
            std::ofstream output(indexPath(path), std::ios::binary | std::ios::trunc);
            if (!output) return failure("Playback index could not be created", indexPath(path).string());
            output << index.dump() << '\n';
            if (!output) return failure("Playback index could not be written", indexPath(path).string());
        }
        checkpoints = std::move(rebuilt);
        {
            std::lock_guard lock(historical_mutex);
            findings = std::move(rebuilt_findings);
            audits = std::move(rebuilt_audits);
        }
        duration_us.store(previous_elapsed, std::memory_order_release);
        return core::makeSuccess();
    }

    core::Result<void> openAt(std::uint64_t offset) {
        stream.close();
        stream.clear();
        stream.open(path, std::ios::binary);
        if (!stream) return failure("Playback session could not be reopened", path.string());
        stream.seekg(static_cast<std::streamoff>(offset));
        if (!stream) return failure("Playback session seek failed", path.string());
        next.reset();
        return core::makeSuccess();
    }

    core::Result<std::optional<Json>> readNextObd() {
        std::string line;
        while (std::getline(stream, line)) {
            const auto parsed = session::parseSessionRecord(line);
            if (!parsed) return tl::make_unexpected(parsed.error());
            if (parsed->at("type") == "obd_message") return std::optional<Json>{*parsed};
        }
        return std::optional<Json>{std::nullopt};
    }
};

PlaybackDataSource::PlaybackDataSource(std::shared_ptr<core::IClock> clock)
    : AsyncDataSource(core::DataSourceType::Playback),
      playback_(std::make_unique<Impl>(clock ? std::move(clock) : std::make_shared<core::SystemClockSource>())) {}

PlaybackDataSource::~PlaybackDataSource() {
    synchronizeWorker([this] {
        ++playback_->generation;
        playback_->state.store(PlaybackState::Stopped, std::memory_order_release);
    });
    cancelDelayedWorkerOperations();
    synchronizeWorker({});
}

void PlaybackDataSource::startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) {
    const auto* playback_config = std::get_if<core::PlaybackConfig>(&config);
    if (!playback_config || playback_config->session_file_path.empty()) {
        completion(failure("Playback requires a session file path"));
        return;
    }
    if (playback_config->speed_multiplier != 0.5 && playback_config->speed_multiplier != 1.0 &&
        playback_config->speed_multiplier != 2.0 && playback_config->speed_multiplier != 5.0) {
        completion(failure("Playback speed must be 0.5x, 1x, 2x, or 5x"));
        return;
    }
    playback_->path = playback_config->session_file_path;
    playback_->speed = playback_config->speed_multiplier;
    playback_->loop = playback_config->loop;
    if (auto result = playback_->validateAndIndex(); !result) { completion(std::move(result)); return; }
    if (auto result = playback_->openAt(0); !result) { completion(std::move(result)); return; }
    playback_->position_us.store(0, std::memory_order_release);
    playback_->state.store(PlaybackState::Paused, std::memory_order_release);
    ++playback_->generation;
    completion(core::makeSuccess());
}

void PlaybackDataSource::startDisconnect(core::CompletionCallback completion) {
    ++playback_->generation;
    playback_->state.store(PlaybackState::Stopped, std::memory_order_release);
    playback_->next.reset();
    playback_->stream.close();
    completion(core::makeSuccess());
}

void PlaybackDataSource::startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) {
    if (request.mode == 0x04) {
        completion(core::makeError(core::ErrorCode::DiagnosticsUnsupported,
            "Mode 04 is unavailable during session playback"));
        return;
    }
    completion(core::makeError(core::ErrorCode::CoreInvalidState,
        "Playback streams recorded responses and does not accept live OBD requests"));
}

void PlaybackDataSource::play(core::CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        if (connectionState() != core::ConnectionState::Ready) {
            if (completion) completion(core::makeError(core::ErrorCode::CoreInvalidState, "Playback source is not ready"));
            return;
        }
        playback_->play_anchor = playback_->clock->monotonicNow();
        playback_->play_anchor_us = playback_->position_us.load(std::memory_order_acquire);
        playback_->session_origin = playback_->play_anchor - Microseconds{playback_->play_anchor_us};
        playback_->state.store(PlaybackState::Playing, std::memory_order_release);
        const auto generation = ++playback_->generation;
        processDue(generation);
        if (completion) completion(core::makeSuccess());
    });
}

void PlaybackDataSource::pause(core::CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        playback_->position_us.store(playback_->currentVirtualPosition(), std::memory_order_release);
        ++playback_->generation;
        playback_->state.store(PlaybackState::Paused, std::memory_order_release);
        if (completion) completion(core::makeSuccess());
    });
}

void PlaybackDataSource::step(core::CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        ++playback_->generation;
        playback_->state.store(PlaybackState::Paused, std::memory_order_release);
        if (!playback_->next) {
            auto record = playback_->readNextObd();
            if (!record) { if (completion) completion(tl::make_unexpected(record.error())); return; }
            if (!*record) { if (completion) completion(core::makeSuccess()); return; }
            const auto parsed = parseMessage(**record, playback_->clock->monotonicNow());
            if (!parsed) { if (completion) completion(tl::make_unexpected(parsed.error())); return; }
            playback_->next = *parsed;
        }
        playback_->next->message.monotonic_ts = playback_->clock->monotonicNow();
        playback_->position_us.store(playback_->next->elapsed_us, std::memory_order_release);
        publishMessage(playback_->next->message);
        playback_->next.reset();
        if (completion) completion(core::makeSuccess());
    });
}

void PlaybackDataSource::stop(core::CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        ++playback_->generation;
        playback_->state.store(PlaybackState::Stopped, std::memory_order_release);
        const auto result = playback_->openAt(0);
        playback_->position_us.store(0, std::memory_order_release);
        if (completion) completion(result);
    });
}

void PlaybackDataSource::seek(
    std::chrono::microseconds target,
    std::chrono::steady_clock::duration warmup,
    core::CompletionCallback completion) {
    postToWorker([this, target, warmup, completion = std::move(completion)]() mutable {
        ++playback_->generation;
        const auto duration = playback_->duration_us.load(std::memory_order_acquire);
        const auto target_us = std::clamp<std::int64_t>(target.count(), 0, duration);
        const auto warmup_us = std::max<std::int64_t>(0, std::chrono::duration_cast<Microseconds>(warmup).count());
        const auto start_us = std::max<std::int64_t>(0, target_us - warmup_us);
        const auto checkpoint = std::upper_bound(
            playback_->checkpoints.begin(), playback_->checkpoints.end(), start_us,
            [](std::int64_t value, const Checkpoint& candidate) { return value < candidate.elapsed_us; });
        const auto selected = checkpoint == playback_->checkpoints.begin() ? playback_->checkpoints.begin() : std::prev(checkpoint);
        const auto offset = selected == playback_->checkpoints.end() ? 0U : selected->offset;
        if (auto result = playback_->openAt(offset); !result) { if (completion) completion(std::move(result)); return; }
        playback_->session_origin = playback_->clock->monotonicNow() - Microseconds{target_us};
        while (true) {
            auto record = playback_->readNextObd();
            if (!record) { if (completion) completion(tl::make_unexpected(record.error())); return; }
            if (!*record) break;
            const auto elapsed = (**record).at("elapsed_us").get<std::int64_t>();
            if (elapsed > target_us) {
                const auto parsed = parseMessage(**record, playback_->session_origin + Microseconds{elapsed});
                if (!parsed) { if (completion) completion(tl::make_unexpected(parsed.error())); return; }
                playback_->next = *parsed;
                break;
            }
            if (elapsed >= start_us) {
                const auto parsed = parseMessage(**record, playback_->session_origin + Microseconds{elapsed});
                if (!parsed) { if (completion) completion(tl::make_unexpected(parsed.error())); return; }
                publishMessage(parsed->message);
            }
        }
        playback_->position_us.store(target_us, std::memory_order_release);
        playback_->state.store(PlaybackState::Paused, std::memory_order_release);
        if (completion) completion(core::makeSuccess());
    });
}

void PlaybackDataSource::setSpeedMultiplier(double multiplier, core::CompletionCallback completion) {
    postToWorker([this, multiplier, completion = std::move(completion)]() mutable {
        if (multiplier != 0.5 && multiplier != 1.0 && multiplier != 2.0 && multiplier != 5.0) {
            if (completion) completion(failure("Playback speed must be 0.5x, 1x, 2x, or 5x"));
            return;
        }
        if (playback_->state.load(std::memory_order_acquire) == PlaybackState::Playing) {
            const auto virtual_position = playback_->currentVirtualPosition();
            playback_->position_us.store(virtual_position, std::memory_order_release);
            playback_->play_anchor = playback_->clock->monotonicNow();
            playback_->play_anchor_us = virtual_position;
            playback_->speed = multiplier;
            const auto generation = ++playback_->generation;
            processDue(generation);
        } else {
            playback_->speed = multiplier;
        }
        if (completion) completion(core::makeSuccess());
    });
}

void PlaybackDataSource::poll() {
    postToWorker([this] { processDue(playback_->generation); });
}

void PlaybackDataSource::processDue(std::uint64_t generation) {
    if (generation != playback_->generation || playback_->state.load(std::memory_order_acquire) != PlaybackState::Playing) return;
    while (true) {
        if (!playback_->next) {
            auto record = playback_->readNextObd();
            if (!record) { playback_->state.store(PlaybackState::Stopped, std::memory_order_release); return; }
            if (!*record) {
                if (!playback_->loop) {
                    playback_->position_us.store(playback_->duration_us.load(std::memory_order_acquire), std::memory_order_release);
                    playback_->state.store(PlaybackState::Stopped, std::memory_order_release);
                    return;
                }
                static_cast<void>(playback_->openAt(0));
                playback_->position_us.store(0, std::memory_order_release);
                playback_->play_anchor = playback_->clock->monotonicNow();
                playback_->play_anchor_us = 0;
                playback_->session_origin = playback_->play_anchor;
                continue;
            }
            const auto elapsed = (**record).at("elapsed_us").get<std::int64_t>();
            const auto parsed = parseMessage(**record, playback_->session_origin + Microseconds{elapsed});
            if (!parsed) { playback_->state.store(PlaybackState::Stopped, std::memory_order_release); return; }
            playback_->next = *parsed;
        }
        const auto delta = playback_->next->elapsed_us - playback_->play_anchor_us;
        const auto due = playback_->play_anchor + Microseconds{static_cast<std::int64_t>(static_cast<double>(delta) / playback_->speed)};
        if (playback_->clock->monotonicNow() < due) break;
        playback_->position_us.store(playback_->next->elapsed_us, std::memory_order_release);
        playback_->next->message.monotonic_ts = playback_->session_origin + Microseconds{playback_->next->elapsed_us};
        publishMessage(playback_->next->message);
        playback_->next.reset();
    }
    scheduleWake(generation);
}

void PlaybackDataSource::scheduleWake(std::uint64_t generation) {
    postAfterToWorker(std::chrono::milliseconds{2}, [this, generation] { processDue(generation); });
}

PlaybackState PlaybackDataSource::playbackState() const noexcept { return playback_->state.load(std::memory_order_acquire); }
std::chrono::microseconds PlaybackDataSource::position() const noexcept { return Microseconds{playback_->position_us.load(std::memory_order_acquire)}; }
std::chrono::microseconds PlaybackDataSource::duration() const noexcept { return Microseconds{playback_->duration_us.load(std::memory_order_acquire)}; }
bool PlaybackDataSource::indexWasRebuilt() const noexcept { return playback_->index_rebuilt.load(std::memory_order_acquire); }

std::vector<session::HistoricalSessionRecord> PlaybackDataSource::historicalFindings() const {
    std::lock_guard lock(playback_->historical_mutex);
    return playback_->findings;
}

std::vector<session::HistoricalSessionRecord> PlaybackDataSource::historicalMode04Audits() const {
    std::lock_guard lock(playback_->historical_mutex);
    return playback_->audits;
}

} // namespace revdash::drivers
