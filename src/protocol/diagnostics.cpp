#include "revdash/protocol/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "revdash/protocol/mode01.hpp"

namespace revdash::protocol {
namespace {

using core::ErrorCode;
using core::ObdMessage;

constexpr std::uint8_t kNegativeResponse = 0x7F;

[[nodiscard]] tl::unexpected<core::Error> malformed(std::string message) {
    return core::makeError(ErrorCode::ProtocolMalformedResponse, std::move(message));
}

[[nodiscard]] core::Result<void> validatePositiveService(
    const ObdMessage& message,
    std::uint8_t positive_service,
    std::string_view name
) {
    const auto payload = message.payload();
    if (payload.empty()) {
        return malformed(std::string{name} + " response is truncated before the service byte");
    }
    if (payload[0] == kNegativeResponse) {
        return core::makeError(ErrorCode::ProtocolNegativeResponse, std::string{name} + " request was rejected by the ECU");
    }
    if (payload[0] != positive_service) {
        return malformed(std::string{name} + " response has an incorrect positive service byte");
    }
    return {};
}

[[nodiscard]] core::Result<std::vector<core::DtcRecord>> decodeDtcs(
    const ObdMessage& message,
    std::uint8_t positive_service,
    core::DtcStatus status,
    std::string_view name
) {
    if (const auto validation = validatePositiveService(message, positive_service, name); !validation.has_value()) {
        return tl::make_unexpected(validation.error());
    }
    const auto bytes = message.payload().subspan(1);
    if ((bytes.size() % 2U) != 0U) {
        return malformed(std::string{name} + " response contains a truncated DTC pair");
    }

    std::vector<core::DtcRecord> records;
    records.reserve(bytes.size() / 2U);
    for (std::size_t index = 0; index < bytes.size(); index += 2U) {
        if (bytes[index] == 0U && bytes[index + 1U] == 0U) {
            continue;
        }
        records.push_back(core::DtcRecord{
            .code = decodeDtc(bytes[index], bytes[index + 1U]),
            .status = status,
            .ecu_address = message.ecu_address
        });
    }
    return records;
}

[[nodiscard]] bool isVinCharacter(std::uint8_t character) noexcept {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z' && character != 'I' && character != 'O' && character != 'Q');
}

[[nodiscard]] bool isPrintableAscii(std::uint8_t character) noexcept {
    return character >= 0x20U && character <= 0x7EU;
}

[[nodiscard]] std::string asAscii(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string asUpperHex(std::span<const std::uint8_t> bytes) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

[[nodiscard]] core::Result<void> validateMode09Header(
    const ObdMessage& message,
    std::uint8_t requested_pid
) {
    if (const auto validation = validatePositiveService(message, 0x49, "Mode 09"); !validation.has_value()) {
        return validation;
    }
    const auto payload = message.payload();
    if (payload.size() < 2U) {
        return malformed("Mode 09 response is truncated before its PID echo");
    }
    if (payload[1] != requested_pid) {
        return malformed("Mode 09 response PID does not match the requested PID");
    }
    return {};
}

} // namespace

std::string decodeDtc(std::uint8_t first_byte, std::uint8_t second_byte) {
    constexpr std::array<char, 4> system_letters{'P', 'C', 'B', 'U'};
    std::string code(5, '0');
    code[0] = system_letters[(first_byte >> 6U) & 0x03U];
    code[1] = static_cast<char>('0' + ((first_byte >> 4U) & 0x03U));
    constexpr std::array<char, 16> hex_digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    code[2] = hex_digits[first_byte & 0x0FU];
    code[3] = hex_digits[(second_byte >> 4U) & 0x0FU];
    code[4] = hex_digits[second_byte & 0x0FU];
    return code;
}

core::Result<std::vector<core::DtcRecord>> decodeStoredDtcs(const ObdMessage& message) {
    return decodeDtcs(message, 0x43, core::DtcStatus::Confirmed, "Mode 03");
}

core::Result<std::vector<core::DtcRecord>> decodePendingDtcs(const ObdMessage& message) {
    return decodeDtcs(message, 0x47, core::DtcStatus::Pending, "Mode 07");
}

std::vector<core::DtcRecord> deduplicateDtcs(std::span<const core::DtcRecord> records) {
    std::vector<core::DtcRecord> unique;
    unique.reserve(records.size());
    for (const auto& record : records) {
        if (std::find(unique.begin(), unique.end(), record) == unique.end()) {
            unique.push_back(record);
        }
    }
    return unique;
}

core::Result<core::FreezeFrame> decodeFreezeFrameZero(
    const ObdMessage& message,
    std::uint8_t requested_pid,
    std::string dtc_code
) {
    if (dtc_code.empty()) {
        return core::makeError(core::ErrorCode::DiagnosticsUnsupported, "Freeze-frame decoding requires its associated DTC code");
    }
    if (const auto validation = validatePositiveService(message, 0x42, "Mode 02"); !validation.has_value()) {
        return tl::make_unexpected(validation.error());
    }
    const auto payload = message.payload();
    if (payload.size() < 2U) {
        return malformed("Mode 02 response is truncated before its PID echo");
    }
    if (payload[1] != requested_pid) {
        return malformed("Mode 02 response PID does not match the requested PID");
    }
    if (findMode01PidDescriptor(requested_pid) == nullptr) {
        return core::makeError(core::ErrorCode::DiagnosticsUnsupported, "Mode 02 PID is not supported by the RevDash Mode 01 decoder");
    }

    auto mode01_message = message;
    mode01_message.data[0] = 0x41;
    const auto samples = decodeMode01Response(mode01_message, requested_pid);
    if (!samples.has_value()) {
        return tl::make_unexpected(samples.error());
    }
    return core::FreezeFrame{
        .dtc_code = std::move(dtc_code),
        .frame_number = 0,
        .timestamp = message.monotonic_ts,
        .samples = *samples
    };
}

core::Result<void> parseClearDiagnosticResponse(const ObdMessage& message) {
    if (const auto validation = validatePositiveService(message, 0x44, "Mode 04"); !validation.has_value()) {
        return validation;
    }
    if (message.payload().size() != 1U) {
        return malformed("Mode 04 positive response contains unexpected data");
    }
    return {};
}

core::Result<core::EcuMetadata> decodeMode09Metadata(const ObdMessage& message, std::uint8_t requested_pid) {
    if (const auto validation = validateMode09Header(message, requested_pid); !validation.has_value()) {
        return tl::make_unexpected(validation.error());
    }
    const auto data = message.payload().subspan(2);
    core::EcuMetadata metadata{.ecu_address = message.ecu_address};
    switch (requested_pid) {
        case 0x02: {
            if (data.size() != 18U || data[0] == 0U) {
                return malformed("Mode 09 VIN response must contain one non-zero record number and 17 characters");
            }
            if (!std::all_of(data.begin() + 1, data.end(), isVinCharacter)) {
                return malformed("Mode 09 VIN contains invalid characters");
            }
            metadata.vin = asAscii(data.subspan(1));
            break;
        }
        case 0x04: {
            constexpr std::size_t kCalibrationRecordSize = 17;
            if (data.empty() || (data.size() % kCalibrationRecordSize) != 0U) {
                return malformed("Mode 09 calibration-ID response contains incomplete records");
            }
            for (std::size_t offset = 0; offset < data.size(); offset += kCalibrationRecordSize) {
                const auto record = data.subspan(offset, kCalibrationRecordSize);
                if (record[0] == 0U || !std::all_of(record.begin() + 1, record.end(), isPrintableAscii)) {
                    return malformed("Mode 09 calibration-ID record is invalid");
                }
                metadata.calibration_ids.push_back(asAscii(record.subspan(1)));
            }
            break;
        }
        case 0x06: {
            constexpr std::size_t kCvnRecordSize = 5;
            if (data.empty() || (data.size() % kCvnRecordSize) != 0U) {
                return malformed("Mode 09 CVN response contains incomplete records");
            }
            for (std::size_t offset = 0; offset < data.size(); offset += kCvnRecordSize) {
                const auto record = data.subspan(offset, kCvnRecordSize);
                if (record[0] == 0U) {
                    return malformed("Mode 09 CVN record has an invalid record number");
                }
                metadata.cvns.push_back(asUpperHex(record.subspan(1)));
            }
            break;
        }
        default:
            return core::makeError(core::ErrorCode::DiagnosticsUnsupported, "Mode 09 PID is not implemented by RevDash");
    }
    return metadata;
}

core::Result<void> mergeMode09Metadata(core::EcuMetadata& target, const core::EcuMetadata& addition) {
    if (target.ecu_address.has_value() && addition.ecu_address.has_value() && target.ecu_address != addition.ecu_address) {
        return core::makeError(core::ErrorCode::ProtocolMalformedResponse, "Cannot merge Mode 09 metadata from different ECUs");
    }
    if (!target.ecu_address.has_value()) {
        target.ecu_address = addition.ecu_address;
    }
    if (!addition.vin.empty()) {
        if (!target.vin.empty() && target.vin != addition.vin) {
            return malformed("Conflicting VIN values were received from one ECU");
        }
        target.vin = addition.vin;
    }
    target.calibration_ids.insert(target.calibration_ids.end(), addition.calibration_ids.begin(), addition.calibration_ids.end());
    target.cvns.insert(target.cvns.end(), addition.cvns.begin(), addition.cvns.end());
    return {};
}

} // namespace revdash::protocol
