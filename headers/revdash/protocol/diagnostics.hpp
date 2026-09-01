#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "revdash/core/diagnostic_types.hpp"
#include "revdash/core/types.hpp"

namespace revdash::protocol {

[[nodiscard]] std::string decodeDtc(std::uint8_t first_byte, std::uint8_t second_byte);

[[nodiscard]] core::Result<std::vector<core::DtcRecord>> decodeStoredDtcs(
    const core::ObdMessage& message
);
[[nodiscard]] core::Result<std::vector<core::DtcRecord>> decodePendingDtcs(
    const core::ObdMessage& message
);

// Removes duplicate code/status/ECU records while retaining distinct ECU provenance.
[[nodiscard]] std::vector<core::DtcRecord> deduplicateDtcs(
    std::span<const core::DtcRecord> records
);

// Decodes a complete Mode 02 response for freeze-frame number zero using the
// Mode 01 PID decoder. The caller supplies the DTC associated with the frame.
[[nodiscard]] core::Result<core::FreezeFrame> decodeFreezeFrameZero(
    const core::ObdMessage& message,
    std::uint8_t requested_pid,
    std::string dtc_code
);

[[nodiscard]] constexpr core::ObdRequest makeClearDiagnosticRequest() noexcept {
    return core::ObdRequest{.mode = 0x04, .pid = 0x00};
}

[[nodiscard]] core::Result<void> parseClearDiagnosticResponse(
    const core::ObdMessage& message
);

// Parses one complete Mode 09 logical response. CALID and CVN payloads may
// include multiple record-number/data groups in the same logical message.
[[nodiscard]] core::Result<core::EcuMetadata> decodeMode09Metadata(
    const core::ObdMessage& message,
    std::uint8_t requested_pid
);

// Combines records from the same ECU only, preserving their separate VIN,
// calibration-ID, and CVN collections.
[[nodiscard]] core::Result<void> mergeMode09Metadata(
    core::EcuMetadata& target,
    const core::EcuMetadata& addition
);

} // namespace revdash::protocol
