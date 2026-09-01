#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "revdash/core/telemetry_types.hpp"

namespace revdash::protocol {

enum class Mode01SchedulerPriority : std::uint8_t {
    Fast,
    Normal,
    Slow,
    Discovery
};

using Mode01Decoder = core::Result<std::vector<core::TelemetrySample>> (*) (
    std::span<const std::uint8_t> data,
    const core::ObdMessage& message
);

struct Mode01PidDescriptor {
    std::uint8_t pid;
    std::uint8_t expected_data_length;
    std::string_view canonical_unit;
    Mode01SchedulerPriority priority;
    Mode01Decoder decoder;
    double minimum_value;
    double maximum_value;
    std::chrono::milliseconds stale_after;
};

[[nodiscard]] std::span<const Mode01PidDescriptor> mode01PidCatalog() noexcept;
[[nodiscard]] const Mode01PidDescriptor* findMode01PidDescriptor(std::uint8_t pid) noexcept;

// Decodes a complete logical J1979 Mode 01 response: 0x41, requested PID, then data.
[[nodiscard]] core::Result<std::vector<core::TelemetrySample>> decodeMode01Response(
    const core::ObdMessage& message,
    std::uint8_t requested_pid
);

class SupportedMode01Pids {
public:
    [[nodiscard]] bool supports(std::uint8_t pid) const noexcept;

    // Stores the 32 PID support flags returned for one base PID (00, 20, 40, ...).
    [[nodiscard]] core::Result<void> applyBitmap(
        std::uint8_t base_pid,
        std::span<const std::uint8_t, 4> bitmap
    );

private:
    std::array<bool, 256> supported_{};
};

// Requests only catalogued data PIDs reported as supported by the ECU.
[[nodiscard]] std::vector<core::ObdRequest> buildMode01QueryFilter(
    const SupportedMode01Pids& supported
);

// Starts at PID 00 and continues to each catalogued bitmap range only when the
// preceding bitmap advertises that the next range is available.
[[nodiscard]] std::vector<core::ObdRequest> buildSupportedPidDiscoveryRequests(
    const SupportedMode01Pids& supported
);

} // namespace revdash::protocol
