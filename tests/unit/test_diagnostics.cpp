#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "revdash/protocol/diagnostics.hpp"

using namespace revdash;

namespace {

core::ObdMessage message(std::span<const std::uint8_t> bytes, std::uint32_t ecu = 0x7E8) {
    const auto result = core::ObdMessage::create(
        core::DataSourceType::Synthetic,
        core::EcuAddress{ecu},
        bytes,
        41,
        core::MonotonicTimePoint{std::chrono::seconds{12}},
        core::UtcTimePoint{std::chrono::seconds{1'700'000'012}}
    );
    REQUIRE(result.has_value());
    return *result;
}

core::ObdMessage message(std::initializer_list<std::uint8_t> bytes, std::uint32_t ecu = 0x7E8) {
    return message(std::span<const std::uint8_t>{bytes.begin(), bytes.size()}, ecu);
}

void appendAscii(std::vector<std::uint8_t>& bytes, std::string_view text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}

} // namespace

TEST_CASE("SAE DTC bitfields convert to every system prefix", "[dtc_codec]") {
    REQUIRE(protocol::decodeDtc(0x03, 0x00) == "P0300");
    REQUIRE(protocol::decodeDtc(0x53, 0x45) == "C1345");
    REQUIRE(protocol::decodeDtc(0xAA, 0xBC) == "B2ABC");
    REQUIRE(protocol::decodeDtc(0xFD, 0xEF) == "U3DEF");
}

TEST_CASE("Mode 03 decodes stored DTCs and omits padding", "[dtc_codec]") {
    const auto decoded = protocol::decodeStoredDtcs(message({0x43, 0x03, 0x00, 0x53, 0x45, 0x00, 0x00}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 2);
    REQUIRE(decoded->at(0).code == "P0300");
    REQUIRE(decoded->at(0).status == core::DtcStatus::Confirmed);
    REQUIRE(decoded->at(0).ecu_address == core::EcuAddress{0x7E8});
    REQUIRE(decoded->at(1).code == "C1345");
}

TEST_CASE("Mode 07 decodes pending DTCs", "[dtc_codec]") {
    const auto decoded = protocol::decodePendingDtcs(message({0x47, 0xAA, 0xBC}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 1);
    REQUIRE(decoded->front().code == "B2ABC");
    REQUIRE(decoded->front().status == core::DtcStatus::Pending);
}

TEST_CASE("DTC deduplication retains status and ECU provenance", "[dtc_codec]") {
    const auto first = protocol::decodeStoredDtcs(message({0x43, 0x03, 0x00}, 0x7E8));
    const auto duplicate = protocol::decodeStoredDtcs(message({0x43, 0x03, 0x00}, 0x7E8));
    const auto second_ecu = protocol::decodeStoredDtcs(message({0x43, 0x03, 0x00}, 0x7E9));
    const auto pending = protocol::decodePendingDtcs(message({0x47, 0x03, 0x00}, 0x7E8));
    REQUIRE(first.has_value());
    REQUIRE(duplicate.has_value());
    REQUIRE(second_ecu.has_value());
    REQUIRE(pending.has_value());

    std::vector<core::DtcRecord> all{first->front(), duplicate->front(), second_ecu->front(), pending->front()};
    const auto unique = protocol::deduplicateDtcs(all);
    REQUIRE(unique.size() == 3);
    REQUIRE(unique.at(0).ecu_address == core::EcuAddress{0x7E8});
    REQUIRE(unique.at(1).ecu_address == core::EcuAddress{0x7E9});
    REQUIRE(unique.at(2).status == core::DtcStatus::Pending);
}

TEST_CASE("DTC services reject malformed and negative responses", "[dtc_codec]") {
    const auto odd_length = protocol::decodeStoredDtcs(message({0x43, 0x03}));
    REQUIRE_FALSE(odd_length.has_value());
    REQUIRE(odd_length.error().code == "Protocol.MalformedResponse");

    const auto wrong_service = protocol::decodePendingDtcs(message({0x43, 0x03, 0x00}));
    REQUIRE_FALSE(wrong_service.has_value());
    REQUIRE(wrong_service.error().code == "Protocol.MalformedResponse");

    const auto negative = protocol::decodeStoredDtcs(message({0x7F, 0x03, 0x12}));
    REQUIRE_FALSE(negative.has_value());
    REQUIRE(negative.error().code == "Protocol.NegativeResponse");
}

TEST_CASE("Mode 02 decodes frame-zero telemetry using Mode 01 decoders", "[mode02]") {
    const auto frame = protocol::decodeFreezeFrameZero(message({0x42, 0x0C, 0x1A, 0xF8}), 0x0C, "P0300");
    REQUIRE(frame.has_value());
    REQUIRE(frame->dtc_code == "P0300");
    REQUIRE(frame->frame_number == 0);
    REQUIRE(frame->timestamp == core::MonotonicTimePoint{std::chrono::seconds{12}});
    REQUIRE(frame->samples.size() == 1);
    REQUIRE(frame->samples.front().metric_id == core::MetricId::Rpm);
    REQUIRE(frame->samples.front().value == 1726.0);

    const auto unsupported = protocol::decodeFreezeFrameZero(message({0x42, 0x01, 0x00}), 0x01, "P0300");
    REQUIRE_FALSE(unsupported.has_value());
    REQUIRE(unsupported.error().code == "Diagnostics.Unsupported");

    const auto missing_dtc = protocol::decodeFreezeFrameZero(message({0x42, 0x0C, 0x1A, 0xF8}), 0x0C, "");
    REQUIRE_FALSE(missing_dtc.has_value());
    REQUIRE(missing_dtc.error().code == "Diagnostics.Unsupported");
}

TEST_CASE("Mode 04 builds and validates clear-diagnostic requests", "[mode04]") {
    const auto request = protocol::makeClearDiagnosticRequest();
    REQUIRE(request.mode == 0x04);
    REQUIRE(request.pid == 0x00);
    REQUIRE(request.extra_payload().empty());

    REQUIRE(protocol::parseClearDiagnosticResponse(message({0x44})).has_value());
    const auto negative = protocol::parseClearDiagnosticResponse(message({0x7F, 0x04, 0x22}));
    REQUIRE_FALSE(negative.has_value());
    REQUIRE(negative.error().code == "Protocol.NegativeResponse");

    const auto malformed = protocol::parseClearDiagnosticResponse(message({0x44, 0x00}));
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error().code == "Protocol.MalformedResponse");
}

TEST_CASE("Mode 09 parses VIN and preserves its ECU source", "[mode09]") {
    std::vector<std::uint8_t> bytes{0x49, 0x02, 0x01};
    appendAscii(bytes, "1HGCR2F83HA000000");
    const auto metadata = protocol::decodeMode09Metadata(message(bytes, 0x7E9), 0x02);
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->vin == "1HGCR2F83HA000000");
    REQUIRE(metadata->ecu_address == core::EcuAddress{0x7E9});
}

TEST_CASE("Mode 09 parses multiple calibration-ID and CVN records", "[mode09]") {
    std::vector<std::uint8_t> calibration{0x49, 0x04, 0x01};
    appendAscii(calibration, "CALID00000000001");
    calibration.push_back(0x02);
    appendAscii(calibration, "CALID00000000002");
    const auto calibration_metadata = protocol::decodeMode09Metadata(message(calibration), 0x04);
    REQUIRE(calibration_metadata.has_value());
    REQUIRE(calibration_metadata->calibration_ids == std::vector<std::string>{"CALID00000000001", "CALID00000000002"});

    const auto cvn_metadata = protocol::decodeMode09Metadata(message({0x49, 0x06, 0x01, 0xA1, 0xB2, 0xC3, 0xD4, 0x02, 0x01, 0x02, 0x03, 0x04}), 0x06);
    REQUIRE(cvn_metadata.has_value());
    REQUIRE(cvn_metadata->cvns == std::vector<std::string>{"A1B2C3D4", "01020304"});

    auto combined = *calibration_metadata;
    REQUIRE(protocol::mergeMode09Metadata(combined, *cvn_metadata).has_value());
    REQUIRE(combined.calibration_ids.size() == 2);
    REQUIRE(combined.cvns.size() == 2);
}

TEST_CASE("Mode 09 rejects malformed data and cross-ECU metadata merging", "[mode09]") {
    const auto invalid_vin = protocol::decodeMode09Metadata(message({0x49, 0x02, 0x01, 'I', 'H', 'G'}), 0x02);
    REQUIRE_FALSE(invalid_vin.has_value());
    REQUIRE(invalid_vin.error().code == "Protocol.MalformedResponse");

    const auto wrong_pid = protocol::decodeMode09Metadata(message({0x49, 0x04, 0x01, 'A'}), 0x02);
    REQUIRE_FALSE(wrong_pid.has_value());
    REQUIRE(wrong_pid.error().code == "Protocol.MalformedResponse");

    const auto malformed_calibration = protocol::decodeMode09Metadata(message({0x49, 0x04, 0x01, 'A'}), 0x04);
    REQUIRE_FALSE(malformed_calibration.has_value());
    REQUIRE(malformed_calibration.error().code == "Protocol.MalformedResponse");

    core::EcuMetadata first{.ecu_address = core::EcuAddress{0x7E8}};
    const core::EcuMetadata other{.ecu_address = core::EcuAddress{0x7E9}};
    const auto merge = protocol::mergeMode09Metadata(first, other);
    REQUIRE_FALSE(merge.has_value());
    REQUIRE(merge.error().code == "Protocol.MalformedResponse");
}
