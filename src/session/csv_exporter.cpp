#include "revdash/session/csv_exporter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include "revdash/core/metric_aggregator.hpp"
#include "revdash/session/session_recorder.hpp"

namespace revdash::session {
namespace {

using core::MetricId;
using core::SampleQuality;
using Json = nlohmann::json;

struct RecordedSample {
    std::int64_t elapsed_us{0};
    double value{0.0};
    SampleQuality quality{SampleQuality::Unsupported};
};

struct Column {
    MetricId metric;
    std::string_view label;
};

constexpr std::array kMegaLogViewerColumns{
    Column{MetricId::Rpm, "RPM"},
    Column{MetricId::VehicleSpeed, "Speed"},
    Column{MetricId::ThrottlePosition, "TPS"},
    Column{MetricId::Map, "MAP"},
    Column{MetricId::Maf, "MAF"},
    Column{MetricId::EngineLoad, "Engine Load"},
    Column{MetricId::TimingAdvance, "Advance"},
    Column{MetricId::CoolantTemp, "CLT"},
    Column{MetricId::ShortTermFuelTrim1, "STFT1"},
    Column{MetricId::LongTermFuelTrim1, "LTFT1"},
    Column{MetricId::ShortTermFuelTrim2, "STFT2"},
    Column{MetricId::LongTermFuelTrim2, "LTFT2"},
    Column{MetricId::AmbientAirTemp, "IAT"},
    Column{MetricId::FuelLevel, "Fuel Level"},
    Column{MetricId::ModuleVoltage, "Battery Voltage"},
    Column{MetricId::O2Sensor1Voltage, "O2"}
};

constexpr std::array kTunerStudioColumns{
    Column{MetricId::Rpm, "RPM"},
    Column{MetricId::VehicleSpeed, "Vehicle Speed"},
    Column{MetricId::ThrottlePosition, "Throttle"},
    Column{MetricId::Map, "MAP"},
    Column{MetricId::Maf, "MAF"},
    Column{MetricId::EngineLoad, "Load"},
    Column{MetricId::TimingAdvance, "Ignition Advance"},
    Column{MetricId::CoolantTemp, "Coolant"},
    Column{MetricId::ShortTermFuelTrim1, "STFT Bank 1"},
    Column{MetricId::LongTermFuelTrim1, "LTFT Bank 1"},
    Column{MetricId::ShortTermFuelTrim2, "STFT Bank 2"},
    Column{MetricId::LongTermFuelTrim2, "LTFT Bank 2"},
    Column{MetricId::AmbientAirTemp, "Intake Air"},
    Column{MetricId::FuelLevel, "Fuel Level"},
    Column{MetricId::ModuleVoltage, "Battery"},
    Column{MetricId::O2Sensor1Voltage, "O2 Sensor 1"}
};

[[nodiscard]] auto sessionError(std::string message, const std::filesystem::path& context = {}) {
    return core::makeError(core::ErrorCode::SessionInvalidFormat, std::move(message), false, context.string());
}

[[nodiscard]] auto storageError(std::string message, const std::filesystem::path& context = {}) {
    return core::makeError(core::ErrorCode::StorageUnavailable, std::move(message), false, context.string());
}

[[nodiscard]] std::optional<MetricId> parseMetric(std::string_view name) noexcept {
    for (std::size_t index = 0; index < core::kMetricCount; ++index) {
        const auto metric = static_cast<MetricId>(index);
        if (core::toString(metric) == name) return metric;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SampleQuality> parseQuality(std::string_view name) noexcept {
    constexpr std::array qualities{SampleQuality::Valid, SampleQuality::Stale, SampleQuality::Unsupported,
                                   SampleQuality::Dropped, SampleQuality::Invalid};
    for (const auto quality : qualities) {
        if (core::toString(quality) == name) return quality;
    }
    return std::nullopt;
}

[[nodiscard]] std::string unitFor(MetricId metric, UnitSystem units) {
    if (units == UnitSystem::Imperial) {
        switch (metric) {
            case MetricId::VehicleSpeed: return "mph";
            case MetricId::Map: return "psi";
            case MetricId::Maf: return "lb/min";
            case MetricId::CoolantTemp:
            case MetricId::AmbientAirTemp: return "degF";
            default: break;
        }
    }
    return std::string{core::getCanonicalUnit(metric)};
}

[[nodiscard]] double convertedValue(MetricId metric, double value, UnitSystem units) noexcept {
    if (units != UnitSystem::Imperial) return value;
    switch (metric) {
        case MetricId::VehicleSpeed: return value * 0.621371192237334;
        case MetricId::Map: return value * 0.14503773773020923;
        case MetricId::Maf: return value * 0.13227735731092653;
        case MetricId::CoolantTemp:
        case MetricId::AmbientAirTemp: return value * 1.8 + 32.0;
        default: return value;
    }
}

[[nodiscard]] std::string number(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general, 12);
    if (result.ec != std::errc{}) return {};
    return std::string{buffer.data(), result.ptr};
}

[[nodiscard]] std::string seconds(std::int64_t elapsed_us) {
    const auto whole = elapsed_us / 1'000'000;
    auto fraction = elapsed_us % 1'000'000;
    if (fraction == 0) return std::to_string(whole);
    std::array<char, 6> digits{};
    for (std::size_t index = 6; index > 0; --index) {
        digits[index - 1] = static_cast<char>('0' + fraction % 10);
        fraction /= 10;
    }
    std::size_t length = digits.size();
    while (length > 0 && digits[length - 1] == '0') --length;
    return std::to_string(whole) + '.' + std::string{digits.data(), length};
}

[[nodiscard]] std::string csvField(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string{value};
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const auto character : value) {
        if (character == '"') escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string columnHeader(const Column& column, UnitSystem units) {
    return std::string{column.label} + " [" + unitFor(column.metric, units) + ']';
}

[[nodiscard]] std::vector<Column> columnsFor(CsvPreset preset) {
    if (preset == CsvPreset::MegaLogViewer) {
        return {kMegaLogViewerColumns.begin(), kMegaLogViewerColumns.end()};
    }
    if (preset == CsvPreset::TunerStudio) {
        return {kTunerStudioColumns.begin(), kTunerStudioColumns.end()};
    }
    std::vector<Column> columns;
    columns.reserve(core::kMetricCount);
    for (std::size_t index = 0; index < core::kMetricCount; ++index) {
        const auto metric = static_cast<MetricId>(index);
        columns.push_back(Column{metric, core::toString(metric)});
    }
    return columns;
}

[[nodiscard]] std::filesystem::path temporaryPath(const std::filesystem::path& destination) {
    auto path = destination;
    path += ".tmp-" + generateSessionUuid();
    return path;
}

[[nodiscard]] core::Result<void> publish(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = std::error_code{static_cast<int>(GetLastError()), std::system_category()};
        return core::makeError(core::ErrorCode::StorageUnavailable,
                               "Could not atomically replace CSV destination", false,
                               destination.string() + ": " + error.message());
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) return storageError("Could not atomically replace CSV destination", destination);
#endif
    return core::makeSuccess();
}

} // namespace

core::Result<void> exportSessionCsv(
    const std::filesystem::path& session_path,
    const std::filesystem::path& destination_path,
    const CsvExportOptions& options) {
    if (options.interval.count() <= 0) return sessionError("CSV interval must be positive");
    if (session_path.empty() || destination_path.empty() || session_path == destination_path) {
        return storageError("CSV source and destination must be distinct non-empty paths", destination_path);
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(destination_path, equivalent_error) && !equivalent_error &&
        std::filesystem::equivalent(session_path, destination_path, equivalent_error) && !equivalent_error) {
        return storageError("CSV destination resolves to the source session", destination_path);
    }

    std::ifstream input{session_path, std::ios::binary};
    if (!input.is_open()) return storageError("Could not open session for CSV export", session_path);

    std::array<std::vector<RecordedSample>, core::kMetricCount> samples;
    std::int64_t previous_elapsed = -1;
    std::int64_t duration_us = -1;
    bool saw_header = false;
    bool saw_footer = false;
    std::optional<std::string> adapter_name;
    std::size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        const auto parsed = parseSessionRecord(line);
        if (!parsed) return tl::make_unexpected(parsed.error());
        const auto& record = *parsed;
        const auto elapsed = record.at("elapsed_us").get<std::int64_t>();
        if (elapsed < previous_elapsed) return sessionError("Session timestamps are not monotonic", session_path);
        previous_elapsed = elapsed;
        const auto type = record.at("type").get<std::string>();
        if (line_number == 1) {
            if (type != "header") return sessionError("Session header must be the first record", session_path);
            saw_header = true;
            if (record.contains("adapter_metadata") && record.at("adapter_metadata").is_object()) {
                const auto& metadata = record.at("adapter_metadata");
                if (metadata.contains("name") && metadata.at("name").is_string()) {
                    adapter_name = metadata.at("name").get<std::string>();
                }
            }
        } else if (type == "header") {
            return sessionError("Session contains more than one header", session_path);
        }
        if (saw_footer) return sessionError("Session footer must be the last record", session_path);
        if (type == "footer") {
            duration_us = elapsed;
            saw_footer = true;
            continue;
        }
        if (type != "telemetry") continue;
        if (!record.contains("metric") || !record.at("metric").is_string() ||
            !record.contains("quality") || !record.at("quality").is_string() ||
            !record.contains("value") || !record.at("value").is_number()) {
            return sessionError("Malformed telemetry record in session", session_path);
        }
        const auto metric = parseMetric(record.at("metric").get<std::string>());
        const auto quality = parseQuality(record.at("quality").get<std::string>());
        const auto value = record.at("value").get<double>();
        if (!metric || !quality || !std::isfinite(value)) {
            return sessionError("Invalid telemetry value in session", session_path);
        }
        samples[static_cast<std::size_t>(*metric)].push_back(RecordedSample{elapsed, value, *quality});
    }
    if (!input.eof() && input.fail()) return storageError("Could not read session for CSV export", session_path);
    if (!saw_header || !saw_footer || duration_us < 0) {
        return sessionError("CSV export requires a completed session with header and footer", session_path);
    }

    const auto parent = destination_path.has_parent_path() ? destination_path.parent_path()
                                                           : std::filesystem::current_path();
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
        return storageError("CSV destination directory is unavailable", destination_path);
    }
    const auto temporary = temporaryPath(destination_path);
    std::ofstream output{temporary, std::ios::binary | std::ios::out | std::ios::trunc};
    if (!output.is_open()) return storageError("Could not create temporary CSV file", temporary);
    const auto remove_temporary = [&temporary] {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };

    const auto columns = columnsFor(options.preset);
    if (options.preset == CsvPreset::RevDash) {
        output << "# Export Preset," << csvField(toString(options.preset)) << "\n";
        output << "# Source," << csvField(session_path.filename().string()) << "\n";
        output << "# Unit System," << (options.units == UnitSystem::Metric ? "Metric" : "Imperial") << "\n";
        if (adapter_name) output << "# Adapter," << csvField(*adapter_name) << "\n";
    }
    output << "Time [s]";
    for (const auto& column : columns) output << ',' << csvField(columnHeader(column, options.units));
    output << "\n";

    std::array<std::size_t, core::kMetricCount> next{};
    std::array<std::optional<RecordedSample>, core::kMetricCount> latest{};
    for (std::int64_t tick = 0;;) {
        output << seconds(tick);
        for (const auto& column : columns) {
            const auto index = static_cast<std::size_t>(column.metric);
            while (next[index] < samples[index].size() && samples[index][next[index]].elapsed_us <= tick) {
                latest[index] = samples[index][next[index]++];
            }
            output << ',';
            if (latest[index] && latest[index]->quality == SampleQuality::Valid) {
                const auto stale_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    core::MetricAggregator::staleAfter(column.metric)).count();
                if (tick - latest[index]->elapsed_us <= stale_us) {
                    output << number(convertedValue(column.metric, latest[index]->value, options.units));
                }
            }
        }
        output << "\n";
        if (!output) {
            output.close();
            remove_temporary();
            return storageError("Could not write CSV export", destination_path);
        }
        if (duration_us - tick < options.interval.count()) break;
        tick += options.interval.count();
    }
    output.flush();
    if (!output) {
        output.close();
        remove_temporary();
        return storageError("Could not flush CSV export", destination_path);
    }
    output.close();
    if (output.fail()) {
        remove_temporary();
        return storageError("Could not close CSV export", destination_path);
    }
    if (auto result = publish(temporary, destination_path); !result) {
        remove_temporary();
        return result;
    }
    return core::makeSuccess();
}

} // namespace revdash::session
