#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include "revdash/core/types.hpp"

namespace revdash::core {

enum class MetricId : std::uint16_t {
    Rpm = 0,
    VehicleSpeed,
    ThrottlePosition,
    Map,
    Maf,
    EngineLoad,
    TimingAdvance,
    CoolantTemp,
    ShortTermFuelTrim1,
    LongTermFuelTrim1,
    ShortTermFuelTrim2,
    LongTermFuelTrim2,
    AmbientAirTemp,
    FuelLevel,
    ModuleVoltage,
    O2Sensor1Voltage,
    O2Sensor2Voltage,
    O2Sensor1EquivalenceRatio,
    O2Sensor2EquivalenceRatio,
    _Count
};

constexpr std::size_t kMetricCount = static_cast<std::size_t>(MetricId::_Count);

[[nodiscard]] constexpr std::string_view toString(MetricId id) noexcept {
    switch (id) {
        case MetricId::Rpm: return "RPM";
        case MetricId::VehicleSpeed: return "VehicleSpeed";
        case MetricId::ThrottlePosition: return "ThrottlePosition";
        case MetricId::Map: return "MAP";
        case MetricId::Maf: return "MAF";
        case MetricId::EngineLoad: return "EngineLoad";
        case MetricId::TimingAdvance: return "TimingAdvance";
        case MetricId::CoolantTemp: return "CoolantTemp";
        case MetricId::ShortTermFuelTrim1: return "STFT1";
        case MetricId::LongTermFuelTrim1: return "LTFT1";
        case MetricId::ShortTermFuelTrim2: return "STFT2";
        case MetricId::LongTermFuelTrim2: return "LTFT2";
        case MetricId::AmbientAirTemp: return "AmbientAirTemp";
        case MetricId::FuelLevel: return "FuelLevel";
        case MetricId::ModuleVoltage: return "ModuleVoltage";
        case MetricId::O2Sensor1Voltage: return "O2Sensor1Voltage";
        case MetricId::O2Sensor2Voltage: return "O2Sensor2Voltage";
        case MetricId::O2Sensor1EquivalenceRatio: return "O2Sensor1EquivalenceRatio";
        case MetricId::O2Sensor2EquivalenceRatio: return "O2Sensor2EquivalenceRatio";
        case MetricId::_Count: return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view getCanonicalUnit(MetricId id) noexcept {
    switch (id) {
        case MetricId::Rpm: return "rpm";
        case MetricId::VehicleSpeed: return "km/h";
        case MetricId::ThrottlePosition: return "%";
        case MetricId::Map: return "kPa";
        case MetricId::Maf: return "g/s";
        case MetricId::EngineLoad: return "%";
        case MetricId::TimingAdvance: return "deg";
        case MetricId::CoolantTemp: return "degC";
        case MetricId::ShortTermFuelTrim1:
        case MetricId::LongTermFuelTrim1:
        case MetricId::ShortTermFuelTrim2:
        case MetricId::LongTermFuelTrim2: return "%";
        case MetricId::AmbientAirTemp: return "degC";
        case MetricId::FuelLevel: return "%";
        case MetricId::ModuleVoltage: return "V";
        case MetricId::O2Sensor1Voltage:
        case MetricId::O2Sensor2Voltage: return "V";
        case MetricId::O2Sensor1EquivalenceRatio:
        case MetricId::O2Sensor2EquivalenceRatio: return "ratio";
        case MetricId::_Count: return "";
    }
    return "";
}

enum class SampleQuality : std::uint8_t {
    Valid = 0,
    Stale,
    Unsupported,
    Dropped,
    Invalid
};

[[nodiscard]] constexpr std::string_view toString(SampleQuality quality) noexcept {
    switch (quality) {
        case SampleQuality::Valid: return "Valid";
        case SampleQuality::Stale: return "Stale";
        case SampleQuality::Unsupported: return "Unsupported";
        case SampleQuality::Dropped: return "Dropped";
        case SampleQuality::Invalid: return "Invalid";
    }
    return "Unknown";
}

struct TelemetrySample {
    MetricId metric_id{MetricId::Rpm};
    double value{0.0};
    SampleQuality quality{SampleQuality::Unsupported};
    MonotonicTimePoint monotonic_ts{MonotonicClock::now()};
    std::optional<UtcTimePoint> utc_ts{SystemClock::now()};
    std::uint64_t sequence_number{0};
    std::optional<EcuAddress> ecu_address{std::nullopt};

    [[nodiscard]] bool isValid() const noexcept {
        return quality == SampleQuality::Valid;
    }
};

struct TelemetrySnapshot {
    MonotonicTimePoint snapshot_monotonic_ts{MonotonicClock::now()};
    std::optional<UtcTimePoint> snapshot_utc_ts{SystemClock::now()};
    std::uint64_t epoch{0};
    std::array<TelemetrySample, kMetricCount> samples{};

    [[nodiscard]] const TelemetrySample& get(MetricId id) const noexcept {
        const auto idx = static_cast<std::size_t>(id);
        if (idx < kMetricCount) {
            return samples[idx];
        }
        static const TelemetrySample kInvalidSample{
            .metric_id = MetricId::Rpm,
            .value = 0.0,
            .quality = SampleQuality::Invalid
        };
        return kInvalidSample;
    }

    [[nodiscard]] double getValueOrDefault(MetricId id, double fallback) const noexcept {
        const auto& s = get(id);
        return s.isValid() ? s.value : fallback;
    }

    [[nodiscard]] bool isValid(MetricId id) const noexcept {
        return get(id).isValid();
    }

    [[nodiscard]] bool isSupported(MetricId id) const noexcept {
        return get(id).quality != SampleQuality::Unsupported;
    }
};

} // namespace revdash::core
