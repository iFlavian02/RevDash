#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "revdash/protocol/mode01.hpp"

using namespace revdash;

namespace {

core::ObdMessage mode01Message(std::initializer_list<std::uint8_t> bytes) {
    const auto message = core::ObdMessage::create(
        core::DataSourceType::Synthetic,
        core::EcuAddress{0x7E8},
        std::span<const std::uint8_t>{bytes.begin(), bytes.size()},
        17,
        core::MonotonicTimePoint{std::chrono::milliseconds{200}},
        core::UtcTimePoint{std::chrono::seconds{1'700'000'000}}
    );
    REQUIRE(message.has_value());
    return *message;
}

void requireSingle(
    std::uint8_t pid,
    std::initializer_list<std::uint8_t> response,
    core::MetricId metric,
    double expected
) {
    const auto decoded = protocol::decodeMode01Response(mode01Message(response), pid);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 1);
    REQUIRE(decoded->front().metric_id == metric);
    REQUIRE(decoded->front().value == Catch::Approx(expected));
    REQUIRE(decoded->front().quality == core::SampleQuality::Valid);
    REQUIRE(decoded->front().sequence_number == 17);
    REQUIRE(decoded->front().ecu_address == core::EcuAddress{0x7E8});
}

} // namespace

TEST_CASE("Mode 01 PID catalog describes every supported live metric", "[mode01]") {
    const auto catalog = protocol::mode01PidCatalog();
    REQUIRE(catalog.size() == 32);
    for (const auto& descriptor : catalog) {
        REQUIRE(protocol::findMode01PidDescriptor(descriptor.pid) == &descriptor);
        REQUIRE(descriptor.expected_data_length > 0);
        REQUIRE_FALSE(descriptor.canonical_unit.empty());
        REQUIRE(descriptor.stale_after.count() > 0);
    }
    REQUIRE(protocol::findMode01PidDescriptor(0x01) == nullptr);
}

TEST_CASE("Mode 01 decodes standard live telemetry vectors", "[mode01]") {
    requireSingle(0x04, {0x41, 0x04, 0x80}, core::MetricId::EngineLoad, 12800.0 / 255.0);
    requireSingle(0x05, {0x41, 0x05, 0x82}, core::MetricId::CoolantTemp, 90.0);
    requireSingle(0x06, {0x41, 0x06, 0xA0}, core::MetricId::ShortTermFuelTrim1, 25.0);
    requireSingle(0x07, {0x41, 0x07, 0x60}, core::MetricId::LongTermFuelTrim1, -25.0);
    requireSingle(0x08, {0x41, 0x08, 0x80}, core::MetricId::ShortTermFuelTrim2, 0.0);
    requireSingle(0x09, {0x41, 0x09, 0x00}, core::MetricId::LongTermFuelTrim2, -100.0);
    requireSingle(0x0B, {0x41, 0x0B, 0x64}, core::MetricId::Map, 100.0);
    requireSingle(0x0C, {0x41, 0x0C, 0x1A, 0xF8}, core::MetricId::Rpm, 1726.0);
    requireSingle(0x0D, {0x41, 0x0D, 0x58}, core::MetricId::VehicleSpeed, 88.0);
    requireSingle(0x0E, {0x41, 0x0E, 0x98}, core::MetricId::TimingAdvance, 12.0);
    requireSingle(0x11, {0x41, 0x11, 0x80}, core::MetricId::ThrottlePosition, 12800.0 / 255.0);
    requireSingle(0x2F, {0x41, 0x2F, 0x40}, core::MetricId::FuelLevel, 6400.0 / 255.0);
    requireSingle(0x42, {0x41, 0x42, 0x34, 0x50}, core::MetricId::ModuleVoltage, 13.392);
    requireSingle(0x46, {0x41, 0x46, 0x3C}, core::MetricId::AmbientAirTemp, 20.0);
}

TEST_CASE("Mode 01 accepts encoded boundary values", "[mode01]") {
    requireSingle(0x05, {0x41, 0x05, 0x00}, core::MetricId::CoolantTemp, -40.0);
    requireSingle(0x05, {0x41, 0x05, 0xFF}, core::MetricId::CoolantTemp, 215.0);
    requireSingle(0x0C, {0x41, 0x0C, 0xFF, 0xFF}, core::MetricId::Rpm, 16383.75);
    requireSingle(0x42, {0x41, 0x42, 0xFF, 0xFF}, core::MetricId::ModuleVoltage, 65.535);
}

TEST_CASE("Mode 01 keeps narrowband and wideband O2 representations distinct", "[mode01]") {
    for (std::uint8_t sensor = 0; sensor < 8; ++sensor) {
        const auto narrow_pid = static_cast<std::uint8_t>(0x14 + sensor);
        const auto narrow = protocol::decodeMode01Response(mode01Message({0x41, narrow_pid, 0xC8, 0x80}), narrow_pid);
        REQUIRE(narrow.has_value());
        REQUIRE(narrow->size() == 1);
        REQUIRE(narrow->front().metric_id == static_cast<core::MetricId>(static_cast<std::size_t>(core::MetricId::O2Sensor1Voltage) + sensor));
        REQUIRE(narrow->front().value == Catch::Approx(1.0));

        const auto wide_pid = static_cast<std::uint8_t>(0x24 + sensor);
        const auto wide = protocol::decodeMode01Response(mode01Message({0x41, wide_pid, 0x80, 0x00, 0x80, 0x00}), wide_pid);
        REQUIRE(wide.has_value());
        REQUIRE(wide->size() == 2);
        REQUIRE(wide->at(0).metric_id == static_cast<core::MetricId>(static_cast<std::size_t>(core::MetricId::O2Sensor1EquivalenceRatio) + sensor));
        REQUIRE(wide->at(0).value == Catch::Approx(65536.0 / 65535.0));
        REQUIRE(wide->at(1).metric_id == static_cast<core::MetricId>(static_cast<std::size_t>(core::MetricId::O2Sensor1Current) + sensor));
        REQUIRE(wide->at(1).value == Catch::Approx(0.0));
    }
}

TEST_CASE("Mode 01 parses supported PID bitmaps and filters queries", "[mode01]") {
    protocol::SupportedMode01Pids supported;
    const std::array<std::uint8_t, 4> first_range{0x18, 0x18, 0x80, 0x01}; // 04, 05, 0C, 0D, 11, 20
    REQUIRE(supported.applyBitmap(0x00, first_range).has_value());
    REQUIRE(supported.supports(0x04));
    REQUIRE(supported.supports(0x05));
    REQUIRE(supported.supports(0x0C));
    REQUIRE(supported.supports(0x0D));
    REQUIRE(supported.supports(0x11));
    REQUIRE(supported.supports(0x20));
    REQUIRE_FALSE(supported.supports(0x06));

    const auto queries = protocol::buildMode01QueryFilter(supported);
    REQUIRE(queries.size() == 5);
    REQUIRE(queries.at(0).pid == 0x04);
    REQUIRE(queries.at(4).pid == 0x11);

    const auto discovery = protocol::buildSupportedPidDiscoveryRequests(supported);
    REQUIRE(discovery.size() == 2);
    REQUIRE(discovery.at(0).pid == 0x00);
    REQUIRE(discovery.at(1).pid == 0x20);

    const std::array<std::uint8_t, 4> third_range{0x00, 0x00, 0x00, 0x00};
    REQUIRE(supported.applyBitmap(0x40, third_range).has_value());
    REQUIRE_FALSE(supported.applyBitmap(0x01, first_range).has_value());
}

TEST_CASE("Mode 01 rejects malformed, mismatched, unsupported, and negative responses", "[mode01]") {
    const auto truncated = protocol::decodeMode01Response(mode01Message({0x41}), 0x0C);
    REQUIRE_FALSE(truncated.has_value());
    REQUIRE(truncated.error().code == "Protocol.MalformedResponse");

    const auto wrong_mode = protocol::decodeMode01Response(mode01Message({0x42, 0x0C, 0x00, 0x00}), 0x0C);
    REQUIRE_FALSE(wrong_mode.has_value());
    REQUIRE(wrong_mode.error().code == "Protocol.MalformedResponse");

    const auto wrong_pid = protocol::decodeMode01Response(mode01Message({0x41, 0x0D, 0x10}), 0x0C);
    REQUIRE_FALSE(wrong_pid.has_value());
    REQUIRE(wrong_pid.error().code == "Protocol.MalformedResponse");

    const auto wrong_length = protocol::decodeMode01Response(mode01Message({0x41, 0x0C, 0x10}), 0x0C);
    REQUIRE_FALSE(wrong_length.has_value());
    REQUIRE(wrong_length.error().code == "Protocol.MalformedResponse");

    const auto negative = protocol::decodeMode01Response(mode01Message({0x7F, 0x01, 0x12}), 0x0C);
    REQUIRE_FALSE(negative.has_value());
    REQUIRE(negative.error().code == "Protocol.NegativeResponse");

    const auto unknown = protocol::decodeMode01Response(mode01Message({0x41, 0x01, 0x00, 0x00, 0x00, 0x00}), 0x01);
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().code == "Protocol.MalformedResponse");
}
