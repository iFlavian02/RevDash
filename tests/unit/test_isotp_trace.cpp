#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "revdash/protocol/isotp_trace.hpp"

using namespace revdash;

namespace {

constexpr protocol::IsoTpAddress kCan11Address{
    .source = core::EcuAddress{0x7E8, core::EcuAddressFormat::Can11Bit},
    .destination = core::EcuAddress{0x7E0, core::EcuAddressFormat::Can11Bit}
};
constexpr protocol::IsoTpAddress kCan29Address{
    .source = core::EcuAddress{0x18DAF110, core::EcuAddressFormat::Can29Bit},
    .destination = core::EcuAddress{0x18DA10F1, core::EcuAddressFormat::Can29Bit}
};

protocol::IsoTpTraceFrame trace(protocol::IsoTpAddress address, std::initializer_list<std::uint8_t> bytes, std::int64_t milliseconds = 0) {
    protocol::IsoTpTraceFrame frame{.address = address, .timestamp = core::MonotonicTimePoint{std::chrono::milliseconds{milliseconds}}, .length = static_cast<std::uint8_t>(bytes.size())};
    std::copy(bytes.begin(), bytes.end(), frame.data.begin());
    return frame;
}

} // namespace

TEST_CASE("ISO-TP trace parses SF, FF, CF, FC, and CAN address formats", "[isotp_trace]") {
    const auto single = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x03, 0x41, 0x0C, 0x00}));
    REQUIRE(single.has_value());
    REQUIRE(single->type == protocol::IsoTpFrameType::SingleFrame);
    REQUIRE(std::ranges::equal(single->bytes(), std::vector<std::uint8_t>{0x41, 0x0C, 0x00}));
    REQUIRE(single->address.source.format == core::EcuAddressFormat::Can11Bit);

    const auto first = protocol::parseIsoTpTraceFrame(trace(kCan29Address, {0x10, 0x09, 1, 2, 3, 4, 5, 6}));
    REQUIRE(first.has_value());
    REQUIRE(first->type == protocol::IsoTpFrameType::FirstFrame);
    REQUIRE(first->declared_length == 9);
    REQUIRE(first->address.source.format == core::EcuAddressFormat::Can29Bit);

    const auto consecutive = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x21, 7, 8, 9}));
    REQUIRE(consecutive.has_value());
    REQUIRE(consecutive->sequence_number == 1);
    REQUIRE(std::ranges::equal(consecutive->bytes(), std::vector<std::uint8_t>{7, 8, 9}));

    const auto flow_control = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x30, 8, 5}));
    REQUIRE(flow_control.has_value());
    REQUIRE(flow_control->type == protocol::IsoTpFrameType::FlowControl);
    REQUIRE(flow_control->block_size == 8);
    REQUIRE(flow_control->st_min == 5);
}

TEST_CASE("ISO-TP trace reassembles single and multi-frame payloads", "[isotp_trace]") {
    const std::vector<std::uint8_t> single_payload{0x41, 0x0C, 0x1A, 0xF8};
    const auto single_frames = protocol::makeIsoTpTraceFrames(kCan11Address, single_payload);
    REQUIRE(single_frames.has_value());
    REQUIRE(single_frames->size() == 1);
    protocol::IsoTpTraceReassembler reassembler;
    const auto parsed_single = protocol::parseIsoTpTraceFrame(single_frames->front());
    REQUIRE(parsed_single.has_value());
    const auto single_complete = reassembler.accept(*parsed_single);
    REQUIRE(single_complete.has_value());
    REQUIRE(single_complete->has_value());
    REQUIRE(single_complete->value().payload == single_payload);

    std::vector<std::uint8_t> multi_payload(30);
    for (std::size_t index = 0; index < multi_payload.size(); ++index) { multi_payload[index] = static_cast<std::uint8_t>(index); }
    const auto frames = protocol::makeIsoTpTraceFrames(kCan29Address, multi_payload);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 5);
    std::optional<protocol::ReassembledIsoTpPayload> completed;
    for (const auto& raw : *frames) {
        const auto parsed = protocol::parseIsoTpTraceFrame(raw);
        REQUIRE(parsed.has_value());
        const auto result = reassembler.accept(*parsed);
        REQUIRE(result.has_value());
        if (result->has_value()) { completed = std::move(*result); }
    }
    REQUIRE(completed.has_value());
    REQUIRE(completed->address == kCan29Address);
    REQUIRE(completed->payload == multi_payload);
}

TEST_CASE("ISO-TP trace detects malformed lengths, sequence errors, and timeouts", "[isotp_trace]") {
    REQUIRE_FALSE(protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x05, 1, 2})).has_value());
    REQUIRE_FALSE(protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x10, 0x07, 1, 2, 3, 4, 5, 6})).has_value());
    REQUIRE_FALSE(protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x20})).has_value());
    REQUIRE_FALSE(protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x33, 0, 0})).has_value());

    protocol::IsoTpTraceReassembler reassembler{std::chrono::milliseconds{10}};
    const auto first = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x10, 0x09, 1, 2, 3, 4, 5, 6}));
    REQUIRE(first.has_value());
    REQUIRE(reassembler.accept(*first).has_value());

    const auto duplicate = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x22, 7, 8, 9}, 1));
    REQUIRE(duplicate.has_value());
    const auto duplicate_result = reassembler.accept(*duplicate);
    REQUIRE_FALSE(duplicate_result.has_value());
    REQUIRE(duplicate_result.error().code == "Protocol.MalformedResponse");

    REQUIRE(reassembler.accept(*first).has_value());
    const auto late = protocol::parseIsoTpTraceFrame(trace(kCan11Address, {0x21, 7, 8, 9}, 11));
    REQUIRE(late.has_value());
    const auto timeout = reassembler.accept(*late);
    REQUIRE_FALSE(timeout.has_value());
    REQUIRE(timeout.error().code == "Transport.Timeout");
}

TEST_CASE("ISO-TP trace fixture helpers enforce the application payload ceiling and sequence rollover", "[isotp_trace]") {
    const std::vector<std::uint8_t> maximum(core::kMaxIsoTpPayloadBytes, 0xA5);
    const auto maximum_frames = protocol::makeIsoTpTraceFrames(kCan11Address, maximum);
    REQUIRE(maximum_frames.has_value());
    REQUIRE(maximum_frames->at(16).data[0] == 0x20); // CF sequence rolls from 15 to 0.

    const std::vector<std::uint8_t> oversized(core::kMaxIsoTpPayloadBytes + 1U, 0x00);
    const auto rejected = protocol::makeIsoTpTraceFrames(kCan11Address, oversized);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == "Protocol.PayloadTooLarge");
}
