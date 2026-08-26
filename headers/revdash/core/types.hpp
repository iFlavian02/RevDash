#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include "revdash/core/error.hpp"

namespace revdash::core {

constexpr std::string_view kApplicationName = "RevDash";
constexpr std::string_view kApplicationVersion = "0.1.0";

enum class ConnectionState {
    Disconnected,
    Connecting,
    Initializing,
    Ready,
    Reconnecting,
    Disconnecting,
    Faulted
};

[[nodiscard]] constexpr std::string_view toString(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected: return "Disconnected";
        case ConnectionState::Connecting: return "Connecting";
        case ConnectionState::Initializing: return "Initializing";
        case ConnectionState::Ready: return "Ready";
        case ConnectionState::Reconnecting: return "Reconnecting";
        case ConnectionState::Disconnecting: return "Disconnecting";
        case ConnectionState::Faulted: return "Faulted";
    }
    return "Unknown";
}

enum class DataSourceType {
    SerialElm327,
    Synthetic,
    Playback,
    SocketCan
};

[[nodiscard]] constexpr std::string_view toString(DataSourceType type) noexcept {
    switch (type) {
        case DataSourceType::SerialElm327: return "SerialElm327";
        case DataSourceType::Synthetic: return "Synthetic";
        case DataSourceType::Playback: return "Playback";
        case DataSourceType::SocketCan: return "SocketCan";
    }
    return "Unknown";
}

using MonotonicClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using MonotonicTimePoint = MonotonicClock::time_point;
using UtcTimePoint = SystemClock::time_point;

constexpr std::size_t kMaxRawFrameBytes = 64;
constexpr std::size_t kMaxIsoTpPayloadBytes = 4095;
constexpr std::size_t kMaxObdRequestBytes = 16;

struct RawTransportFrame {
    DataSourceType source_type{DataSourceType::Synthetic};
    std::optional<std::uint32_t> ecu_address{std::nullopt};
    MonotonicTimePoint monotonic_ts{MonotonicClock::now()};
    UtcTimePoint utc_ts{SystemClock::now()};
    std::uint64_t sequence_number{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, kMaxRawFrameBytes> data{};

    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept {
        return std::span<const std::uint8_t>(data.data(), length);
    }
};

struct ObdRequest {
    std::uint8_t mode{0x01};
    std::uint8_t pid{0x00};
    std::optional<std::uint32_t> target_ecu{std::nullopt};
    std::uint16_t extra_length{0};
    std::array<std::uint8_t, kMaxObdRequestBytes> payload{};

    [[nodiscard]] std::span<const std::uint8_t> extra_payload() const noexcept {
        return std::span<const std::uint8_t>(payload.data(), extra_length);
    }
};

struct ObdMessage {
    DataSourceType source_type{DataSourceType::Synthetic};
    std::optional<std::uint32_t> ecu_address{std::nullopt};
    MonotonicTimePoint monotonic_ts{MonotonicClock::now()};
    UtcTimePoint utc_ts{SystemClock::now()};
    std::uint64_t sequence_number{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, kMaxIsoTpPayloadBytes> data{};

    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept {
        return std::span<const std::uint8_t>(data.data(), length);
    }

    static Result<ObdMessage> create(
        DataSourceType source,
        std::optional<std::uint32_t> ecu,
        std::span<const std::uint8_t> bytes,
        std::uint64_t seq = 0,
        MonotonicTimePoint mono_ts = MonotonicClock::now(),
        UtcTimePoint utc_ts = SystemClock::now()
    ) {
        if (bytes.size() > kMaxIsoTpPayloadBytes) {
            return makeError(
                ErrorDomain::Protocol,
                "Protocol.PayloadTooLarge",
                "ISO-TP / OBD payload exceeds maximum supported capacity of 4095 bytes",
                false,
                "Payload size: " + std::to_string(bytes.size())
            );
        }

        ObdMessage msg{
            .source_type = source,
            .ecu_address = ecu,
            .monotonic_ts = mono_ts,
            .utc_ts = utc_ts,
            .sequence_number = seq,
            .length = static_cast<std::uint16_t>(bytes.size()),
            .data = {}
        };

        if (!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(), msg.data.begin());
        }

        return msg;
    }
};

} // namespace revdash::core
