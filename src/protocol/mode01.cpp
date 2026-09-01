#include "revdash/protocol/mode01.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace revdash::protocol {
namespace {

using core::ErrorCode;
using core::MetricId;
using core::ObdMessage;
using core::Result;
using core::TelemetrySample;

constexpr std::uint8_t kMode01PositiveResponse = 0x41;
constexpr std::uint8_t kNegativeResponse = 0x7F;
constexpr std::array<MetricId, 8> kO2VoltageMetrics{
    MetricId::O2Sensor1Voltage, MetricId::O2Sensor2Voltage, MetricId::O2Sensor3Voltage,
    MetricId::O2Sensor4Voltage, MetricId::O2Sensor5Voltage, MetricId::O2Sensor6Voltage,
    MetricId::O2Sensor7Voltage, MetricId::O2Sensor8Voltage
};
constexpr std::array<MetricId, 8> kO2RatioMetrics{
    MetricId::O2Sensor1EquivalenceRatio, MetricId::O2Sensor2EquivalenceRatio,
    MetricId::O2Sensor3EquivalenceRatio, MetricId::O2Sensor4EquivalenceRatio,
    MetricId::O2Sensor5EquivalenceRatio, MetricId::O2Sensor6EquivalenceRatio,
    MetricId::O2Sensor7EquivalenceRatio, MetricId::O2Sensor8EquivalenceRatio
};
constexpr std::array<MetricId, 8> kO2CurrentMetrics{
    MetricId::O2Sensor1Current, MetricId::O2Sensor2Current, MetricId::O2Sensor3Current,
    MetricId::O2Sensor4Current, MetricId::O2Sensor5Current, MetricId::O2Sensor6Current,
    MetricId::O2Sensor7Current, MetricId::O2Sensor8Current
};

[[nodiscard]] tl::unexpected<core::Error> malformed(std::string message) {
    return core::makeError(ErrorCode::ProtocolMalformedResponse, std::move(message));
}

[[nodiscard]] Result<std::vector<TelemetrySample>> single(
    MetricId metric,
    double value,
    const ObdMessage& message
) {
    return std::vector<TelemetrySample>{TelemetrySample{
        .metric_id = metric,
        .value = value,
        .quality = core::SampleQuality::Valid,
        .monotonic_ts = message.monotonic_ts,
        .utc_ts = message.utc_ts,
        .sequence_number = message.sequence_number,
        .ecu_address = message.ecu_address
    }};
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeRpm(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(MetricId::Rpm, static_cast<double>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]) / 4.0, message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeSpeed(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(MetricId::VehicleSpeed, data[0], message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeCoolant(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(MetricId::CoolantTemp, static_cast<double>(data[0]) - 40.0, message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeLoad(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(MetricId::EngineLoad, static_cast<double>(data[0]) * 100.0 / 255.0, message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeThrottle(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(MetricId::ThrottlePosition, static_cast<double>(data[0]) * 100.0 / 255.0, message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeFuelTrim(std::span<const std::uint8_t> data, const ObdMessage& message, MetricId metric) {
    return single(metric, (static_cast<double>(data[0]) - 128.0) * 100.0 / 128.0, message);
}

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeStft1(std::span<const std::uint8_t> data, const ObdMessage& message) { return decodeFuelTrim(data, message, MetricId::ShortTermFuelTrim1); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeLtft1(std::span<const std::uint8_t> data, const ObdMessage& message) { return decodeFuelTrim(data, message, MetricId::LongTermFuelTrim1); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeStft2(std::span<const std::uint8_t> data, const ObdMessage& message) { return decodeFuelTrim(data, message, MetricId::ShortTermFuelTrim2); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeLtft2(std::span<const std::uint8_t> data, const ObdMessage& message) { return decodeFuelTrim(data, message, MetricId::LongTermFuelTrim2); }

[[nodiscard]] Result<std::vector<TelemetrySample>> decodeMap(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::Map, data[0], message); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeMaf(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::Maf, static_cast<double>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]) / 100.0, message); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeTiming(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::TimingAdvance, static_cast<double>(data[0]) / 2.0 - 64.0, message); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeAmbient(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::AmbientAirTemp, static_cast<double>(data[0]) - 40.0, message); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeFuelLevel(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::FuelLevel, static_cast<double>(data[0]) * 100.0 / 255.0, message); }
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeVoltage(std::span<const std::uint8_t> data, const ObdMessage& message) { return single(MetricId::ModuleVoltage, static_cast<double>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]) / 1000.0, message); }

template <std::size_t Index>
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeNarrowbandO2(std::span<const std::uint8_t> data, const ObdMessage& message) {
    return single(kO2VoltageMetrics[Index], static_cast<double>(data[0]) * 0.005, message);
}

template <std::size_t Index>
[[nodiscard]] Result<std::vector<TelemetrySample>> decodeWidebandO2(std::span<const std::uint8_t> data, const ObdMessage& message) {
    const auto ratio_raw = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
    const auto current_raw = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[2]) << 8U) | data[3]);
    return std::vector<TelemetrySample>{
        TelemetrySample{.metric_id = kO2RatioMetrics[Index], .value = static_cast<double>(ratio_raw) * 2.0 / 65535.0, .quality = core::SampleQuality::Valid, .monotonic_ts = message.monotonic_ts, .utc_ts = message.utc_ts, .sequence_number = message.sequence_number, .ecu_address = message.ecu_address},
        TelemetrySample{.metric_id = kO2CurrentMetrics[Index], .value = static_cast<double>(current_raw) / 256.0 - 128.0, .quality = core::SampleQuality::Valid, .monotonic_ts = message.monotonic_ts, .utc_ts = message.utc_ts, .sequence_number = message.sequence_number, .ecu_address = message.ecu_address}
    };
}

constexpr std::chrono::milliseconds kFastStale{1'000};
constexpr std::chrono::milliseconds kNormalStale{2'000};
constexpr std::chrono::milliseconds kSlowStale{5'000};
constexpr std::chrono::milliseconds kDiscoveryStale{60'000};

const std::array<Mode01PidDescriptor, 32> kCatalog{{
    {0x00, 4, "bitmap", Mode01SchedulerPriority::Discovery, nullptr, 0.0, 0.0, kDiscoveryStale},
    {0x04, 1, "%", Mode01SchedulerPriority::Normal, decodeLoad, 0.0, 100.0, kNormalStale},
    {0x05, 1, "degC", Mode01SchedulerPriority::Normal, decodeCoolant, -40.0, 215.0, kNormalStale},
    {0x06, 1, "%", Mode01SchedulerPriority::Normal, decodeStft1, -100.0, 100.0, kNormalStale},
    {0x07, 1, "%", Mode01SchedulerPriority::Normal, decodeLtft1, -100.0, 100.0, kNormalStale},
    {0x08, 1, "%", Mode01SchedulerPriority::Normal, decodeStft2, -100.0, 100.0, kNormalStale},
    {0x09, 1, "%", Mode01SchedulerPriority::Normal, decodeLtft2, -100.0, 100.0, kNormalStale},
    {0x0B, 1, "kPa", Mode01SchedulerPriority::Fast, decodeMap, 0.0, 255.0, kFastStale},
    {0x0C, 2, "rpm", Mode01SchedulerPriority::Fast, decodeRpm, 0.0, 16383.75, kFastStale},
    {0x0D, 1, "km/h", Mode01SchedulerPriority::Fast, decodeSpeed, 0.0, 255.0, kFastStale},
    {0x0E, 1, "deg", Mode01SchedulerPriority::Normal, decodeTiming, -64.0, 63.5, kNormalStale},
    {0x11, 1, "%", Mode01SchedulerPriority::Fast, decodeThrottle, 0.0, 100.0, kFastStale},
    {0x14, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<0>, 0.0, 1.275, kNormalStale},
    {0x15, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<1>, 0.0, 1.275, kNormalStale},
    {0x16, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<2>, 0.0, 1.275, kNormalStale},
    {0x17, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<3>, 0.0, 1.275, kNormalStale},
    {0x18, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<4>, 0.0, 1.275, kNormalStale},
    {0x19, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<5>, 0.0, 1.275, kNormalStale},
    {0x1A, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<6>, 0.0, 1.275, kNormalStale},
    {0x1B, 2, "V", Mode01SchedulerPriority::Normal, decodeNarrowbandO2<7>, 0.0, 1.275, kNormalStale},
    {0x20, 4, "bitmap", Mode01SchedulerPriority::Discovery, nullptr, 0.0, 0.0, kDiscoveryStale},
    {0x24, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<0>, 0.0, 2.0, kNormalStale},
    {0x25, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<1>, 0.0, 2.0, kNormalStale},
    {0x26, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<2>, 0.0, 2.0, kNormalStale},
    {0x27, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<3>, 0.0, 2.0, kNormalStale},
    {0x28, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<4>, 0.0, 2.0, kNormalStale},
    {0x29, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<5>, 0.0, 2.0, kNormalStale},
    {0x2A, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<6>, 0.0, 2.0, kNormalStale},
    {0x2B, 4, "ratio/A", Mode01SchedulerPriority::Normal, decodeWidebandO2<7>, 0.0, 2.0, kNormalStale},
    {0x2F, 1, "%", Mode01SchedulerPriority::Slow, decodeFuelLevel, 0.0, 100.0, kSlowStale},
    {0x42, 2, "V", Mode01SchedulerPriority::Slow, decodeVoltage, 0.0, 65.535, kSlowStale},
    {0x46, 1, "degC", Mode01SchedulerPriority::Slow, decodeAmbient, -40.0, 215.0, kSlowStale}
}};

[[nodiscard]] bool isBitmapPid(std::uint8_t pid) noexcept { return pid == 0x00 || pid == 0x20 || pid == 0x40; }

} // namespace

std::span<const Mode01PidDescriptor> mode01PidCatalog() noexcept { return kCatalog; }

const Mode01PidDescriptor* findMode01PidDescriptor(std::uint8_t pid) noexcept {
    const auto found = std::find_if(kCatalog.begin(), kCatalog.end(), [pid](const Mode01PidDescriptor& entry) { return entry.pid == pid; });
    return found == kCatalog.end() ? nullptr : &*found;
}

Result<std::vector<TelemetrySample>> decodeMode01Response(const ObdMessage& message, std::uint8_t requested_pid) {
    const auto payload = message.payload();
    if (payload.empty()) {
        return malformed("Mode 01 response is truncated before the service byte");
    }
    if (payload[0] == kNegativeResponse) {
        return core::makeError(ErrorCode::ProtocolNegativeResponse, "ECU rejected Mode 01 request");
    }
    if (payload.size() < 2) {
        return malformed("Mode 01 response is truncated before the PID echo");
    }
    if (payload[0] != kMode01PositiveResponse) {
        return malformed("Response service is not the positive Mode 01 response (0x41)");
    }
    if (payload[1] != requested_pid) {
        return malformed("Mode 01 response PID does not match the requested PID");
    }
    const auto* descriptor = findMode01PidDescriptor(requested_pid);
    if (descriptor == nullptr) {
        return malformed("Mode 01 response uses a PID outside the supported RevDash catalog");
    }
    const auto data = payload.subspan(2);
    if (data.size() != descriptor->expected_data_length) {
        return malformed("Mode 01 response data length does not match the PID descriptor");
    }
    if (isBitmapPid(requested_pid)) {
        return std::vector<TelemetrySample>{};
    }
    auto decoded = descriptor->decoder(data, message);
    if (!decoded.has_value()) {
        return decoded;
    }
    if (decoded->empty() || !std::isfinite(decoded->front().value) ||
        decoded->front().value < descriptor->minimum_value ||
        decoded->front().value > descriptor->maximum_value) {
        return malformed("Mode 01 response decoded to a value outside the PID descriptor bounds");
    }
    return decoded;
}

bool SupportedMode01Pids::supports(std::uint8_t pid) const noexcept { return supported_[pid]; }

Result<void> SupportedMode01Pids::applyBitmap(std::uint8_t base_pid, std::span<const std::uint8_t, 4> bitmap) {
    if ((base_pid % 0x20U) != 0U) {
        return malformed("Mode 01 supported-PID bitmap base is not aligned to 0x20");
    }
    for (std::size_t bit = 0; bit < 32; ++bit) {
        const auto pid = static_cast<std::uint16_t>(base_pid) + 1U + static_cast<std::uint16_t>(bit);
        if (pid > 0xFFU) {
            break;
        }
        supported_[pid] = (bitmap[bit / 8U] & (0x80U >> (bit % 8U))) != 0U;
    }
    return {};
}

std::vector<core::ObdRequest> buildMode01QueryFilter(const SupportedMode01Pids& supported) {
    std::vector<core::ObdRequest> requests;
    for (const auto& descriptor : kCatalog) {
        if (!isBitmapPid(descriptor.pid) && supported.supports(descriptor.pid)) {
            requests.push_back(core::ObdRequest{.mode = 0x01, .pid = descriptor.pid});
        }
    }
    return requests;
}

std::vector<core::ObdRequest> buildSupportedPidDiscoveryRequests(const SupportedMode01Pids& supported) {
    std::vector<core::ObdRequest> requests{core::ObdRequest{.mode = 0x01, .pid = 0x00}};
    if (supported.supports(0x20)) {
        requests.push_back(core::ObdRequest{.mode = 0x01, .pid = 0x20});
    }
    if (supported.supports(0x40)) {
        requests.push_back(core::ObdRequest{.mode = 0x01, .pid = 0x40});
    }
    return requests;
}

} // namespace revdash::protocol
