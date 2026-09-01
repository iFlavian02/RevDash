#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "revdash/core/types.hpp"

namespace revdash::protocol {

struct IsoTpAddress {
    core::EcuAddress source;
    core::EcuAddress destination;

    [[nodiscard]] friend constexpr bool operator==(const IsoTpAddress&, const IsoTpAddress&) noexcept = default;
};

struct IsoTpAddressHash {
    [[nodiscard]] std::size_t operator()(const IsoTpAddress& address) const noexcept;
};

struct IsoTpTraceFrame {
    IsoTpAddress address;
    core::MonotonicTimePoint timestamp{core::MonotonicClock::now()};
    std::uint8_t length{0};
    std::array<std::uint8_t, 8> data{};

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {data.data(), length};
    }
};

enum class IsoTpFrameType : std::uint8_t { SingleFrame, FirstFrame, ConsecutiveFrame, FlowControl };

struct IsoTpFrame {
    IsoTpFrameType type;
    IsoTpAddress address;
    core::MonotonicTimePoint timestamp;
    std::uint16_t declared_length{0};
    std::uint8_t sequence_number{0};
    std::uint8_t flow_status{0};
    std::uint8_t block_size{0};
    std::uint8_t st_min{0};
    std::array<std::uint8_t, 7> payload{};
    std::uint8_t payload_length{0};

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {payload.data(), payload_length}; }
};

[[nodiscard]] core::Result<IsoTpFrame> parseIsoTpTraceFrame(const IsoTpTraceFrame& frame);

struct ReassembledIsoTpPayload {
    IsoTpAddress address;
    core::MonotonicTimePoint completed_at;
    std::vector<std::uint8_t> payload;
};

class IsoTpTraceReassembler {
public:
    explicit IsoTpTraceReassembler(std::chrono::milliseconds timeout = std::chrono::seconds{1}) noexcept;

    [[nodiscard]] core::Result<std::optional<ReassembledIsoTpPayload>> accept(const IsoTpFrame& frame);
    [[nodiscard]] std::size_t expire(core::MonotonicTimePoint now) noexcept;
    void reset() noexcept;

private:
    struct PendingMessage {
        std::uint16_t declared_length;
        std::uint8_t expected_sequence;
        core::MonotonicTimePoint last_timestamp;
        std::vector<std::uint8_t> payload;
    };

    std::chrono::milliseconds timeout_;
    std::unordered_map<IsoTpAddress, PendingMessage, IsoTpAddressHash> pending_;
};

// Fixture-only helper. It produces deterministic SF/FF/CF trace frames and
// deliberately does not model flow-control traffic or production pacing.
[[nodiscard]] core::Result<std::vector<IsoTpTraceFrame>> makeIsoTpTraceFrames(
    IsoTpAddress address,
    std::span<const std::uint8_t> payload,
    core::MonotonicTimePoint start = core::MonotonicTimePoint{},
    std::chrono::milliseconds spacing = std::chrono::milliseconds{1}
);

} // namespace revdash::protocol
