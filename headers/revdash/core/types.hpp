#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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

class IClock {
public:
    virtual ~IClock() = default;

    [[nodiscard]] virtual MonotonicTimePoint monotonicNow() const noexcept = 0;
    [[nodiscard]] virtual std::optional<UtcTimePoint> utcNow() const noexcept = 0;
};

class SystemClockSource final : public IClock {
public:
    [[nodiscard]] MonotonicTimePoint monotonicNow() const noexcept override {
        return MonotonicClock::now();
    }

    [[nodiscard]] std::optional<UtcTimePoint> utcNow() const noexcept override {
        return SystemClock::now();
    }
};

class ManualClock final : public IClock {
public:
    explicit ManualClock(
        MonotonicTimePoint monotonic_time = MonotonicTimePoint{},
        std::optional<UtcTimePoint> utc_time = UtcTimePoint{}
    ) noexcept
        : monotonic_time_(monotonic_time), utc_time_(utc_time) {}

    [[nodiscard]] MonotonicTimePoint monotonicNow() const noexcept override {
        return monotonic_time_;
    }

    [[nodiscard]] std::optional<UtcTimePoint> utcNow() const noexcept override {
        return utc_time_;
    }

    void advance(MonotonicClock::duration duration) noexcept {
        monotonic_time_ += duration;
        if (utc_time_.has_value()) {
            *utc_time_ += std::chrono::duration_cast<SystemClock::duration>(duration);
        }
    }

    void setMonotonicTime(MonotonicTimePoint time) noexcept {
        monotonic_time_ = time;
    }

    void setUtcTime(std::optional<UtcTimePoint> time) noexcept {
        utc_time_ = time;
    }

private:
    MonotonicTimePoint monotonic_time_;
    std::optional<UtcTimePoint> utc_time_;
};

constexpr std::size_t kMaxRawFrameBytes = 64;
constexpr std::size_t kMaxIsoTpPayloadBytes = 4095;
constexpr std::size_t kMaxObdRequestBytes = 16;

enum class EcuAddressFormat : std::uint8_t {
    Can11Bit,
    Can29Bit,
    Other
};

struct EcuAddress {
    std::uint32_t value{0};
    EcuAddressFormat format{EcuAddressFormat::Can11Bit};

    constexpr EcuAddress() noexcept = default;
    constexpr EcuAddress(std::uint32_t raw_value, EcuAddressFormat address_format = EcuAddressFormat::Can11Bit) noexcept
        : value(raw_value), format(address_format) {}

    [[nodiscard]] friend constexpr bool operator==(const EcuAddress&, const EcuAddress&) noexcept = default;
};

struct EcuAddressHash {
    [[nodiscard]] constexpr std::size_t operator()(const EcuAddress& address) const noexcept {
        return (static_cast<std::size_t>(address.value) << 2U) ^ static_cast<std::size_t>(address.format);
    }
};

struct RawTransportFrame {
    DataSourceType source_type{DataSourceType::Synthetic};
    std::optional<EcuAddress> ecu_address{std::nullopt};
    MonotonicTimePoint monotonic_ts{MonotonicClock::now()};
    std::optional<UtcTimePoint> utc_ts{SystemClock::now()};
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
    std::optional<EcuAddress> target_ecu{std::nullopt};
    std::uint16_t extra_length{0};
    std::array<std::uint8_t, kMaxObdRequestBytes> payload{};

    [[nodiscard]] std::span<const std::uint8_t> extra_payload() const noexcept {
        return std::span<const std::uint8_t>(payload.data(), extra_length);
    }
};

struct ObdMessage {
    DataSourceType source_type{DataSourceType::Synthetic};
    std::optional<EcuAddress> ecu_address{std::nullopt};
    MonotonicTimePoint monotonic_ts{MonotonicClock::now()};
    std::optional<UtcTimePoint> utc_ts{SystemClock::now()};
    std::uint64_t sequence_number{0};
    std::uint16_t length{0};
    std::array<std::uint8_t, kMaxIsoTpPayloadBytes> data{};

    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept {
        return std::span<const std::uint8_t>(data.data(), length);
    }

    static Result<ObdMessage> create(
        DataSourceType source,
        std::optional<EcuAddress> ecu,
        std::span<const std::uint8_t> bytes,
        std::uint64_t seq = 0,
        MonotonicTimePoint mono_ts = MonotonicClock::now(),
        std::optional<UtcTimePoint> utc_ts = SystemClock::now()
    ) {
        if (bytes.size() > kMaxIsoTpPayloadBytes) {
            return makeError(
                ErrorCode::ProtocolPayloadTooLarge,
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

template <>
struct std::hash<revdash::core::EcuAddress> {
    [[nodiscard]] constexpr std::size_t operator()(const revdash::core::EcuAddress& address) const noexcept {
        return revdash::core::EcuAddressHash{}(address);
    }
};
