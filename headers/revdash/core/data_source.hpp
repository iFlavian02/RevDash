#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include "revdash/core/error.hpp"
#include "revdash/core/types.hpp"

namespace revdash::core {

struct SerialConfig {
    std::string port_name{"COM1"};
    std::uint32_t baud_rate{38400};
    std::chrono::milliseconds response_timeout{2000};

    [[nodiscard]] bool operator==(const SerialConfig& other) const noexcept {
        return port_name == other.port_name &&
               baud_rate == other.baud_rate &&
               response_timeout == other.response_timeout;
    }
};

struct SyntheticConfig {
    std::uint32_t deterministic_seed{12345};
    double displacement_liters{2.0};
    std::uint32_t cylinder_count{4};
    double initial_rpm{800.0};
    double ambient_temp_c{20.0};
    bool inject_misfire{false};
    bool inject_vacuum_leak{false};
    bool inject_thermostat_fault{false};
    double noise_std_dev{0.0};
    double packet_dropout_prob{0.0};
    std::chrono::milliseconds response_latency{0};
    bool include_second_ecu{false};

    [[nodiscard]] bool operator==(const SyntheticConfig& other) const noexcept {
        return deterministic_seed == other.deterministic_seed &&
               displacement_liters == other.displacement_liters &&
               cylinder_count == other.cylinder_count &&
               initial_rpm == other.initial_rpm &&
               ambient_temp_c == other.ambient_temp_c &&
               inject_misfire == other.inject_misfire &&
               inject_vacuum_leak == other.inject_vacuum_leak &&
               inject_thermostat_fault == other.inject_thermostat_fault &&
               noise_std_dev == other.noise_std_dev &&
               packet_dropout_prob == other.packet_dropout_prob &&
               response_latency == other.response_latency &&
               include_second_ecu == other.include_second_ecu;
    }
};

struct PlaybackConfig {
    std::string session_file_path;
    double speed_multiplier{1.0};
    bool loop{false};

    [[nodiscard]] bool operator==(const PlaybackConfig& other) const noexcept {
        return session_file_path == other.session_file_path &&
               speed_multiplier == other.speed_multiplier &&
               loop == other.loop;
    }
};

struct SocketCanConfig {
    std::string interface_name{"can0"};
    std::uint32_t bitrate{500000};

    [[nodiscard]] bool operator==(const SocketCanConfig& other) const noexcept {
        return interface_name == other.interface_name && bitrate == other.bitrate;
    }
};

using DataSourceConfig = std::variant<SerialConfig, SyntheticConfig, PlaybackConfig, SocketCanConfig>;

[[nodiscard]] inline DataSourceType getDataSourceType(const DataSourceConfig& config) noexcept {
    return std::visit([](const auto& c) noexcept -> DataSourceType {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SerialConfig>) {
            return DataSourceType::SerialElm327;
        } else if constexpr (std::is_same_v<T, SyntheticConfig>) {
            return DataSourceType::Synthetic;
        } else if constexpr (std::is_same_v<T, PlaybackConfig>) {
            return DataSourceType::Playback;
        } else if constexpr (std::is_same_v<T, SocketCanConfig>) {
            return DataSourceType::SocketCan;
        }
    }, config);
}

using CompletionCallback = std::function<void(Result<void>)>;
using MessageHandler = std::function<void(const ObdMessage&)>;
using StateHandler = std::function<void(ConnectionState, const std::optional<Error>&)>;

class SubscriptionToken {
public:
    using UnsubscribeFn = std::function<void()>;

    SubscriptionToken() noexcept = default;

    explicit SubscriptionToken(UnsubscribeFn unsubscribe_fn)
        : state_(std::make_shared<State>(std::move(unsubscribe_fn))) {}

    ~SubscriptionToken() {
        reset();
    }

    SubscriptionToken(const SubscriptionToken&) = delete;
    SubscriptionToken& operator=(const SubscriptionToken&) = delete;

    SubscriptionToken(SubscriptionToken&& other) noexcept = default;

    SubscriptionToken& operator=(SubscriptionToken&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    void reset() noexcept {
        if (!state_) {
            return;
        }

        UnsubscribeFn unsubscribe_fn;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->active) {
                return;
            }
            state_->active = false;
            unsubscribe_fn = std::move(state_->unsubscribe_fn);
        }
        if (unsubscribe_fn) {
            unsubscribe_fn();
        }
    }

    [[nodiscard]] bool active() const noexcept {
        if (!state_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->active;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return active();
    }

private:
    struct State {
        explicit State(UnsubscribeFn unsubscribe) : unsubscribe_fn(std::move(unsubscribe)) {}

        std::mutex mutex;
        UnsubscribeFn unsubscribe_fn;
        bool active{true};
    };

    std::shared_ptr<State> state_;
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    [[nodiscard]] virtual DataSourceType type() const noexcept = 0;
    [[nodiscard]] virtual ConnectionState connectionState() const noexcept = 0;
    [[nodiscard]] virtual std::optional<DataSourceConfig> currentConfig() const noexcept = 0;

    virtual void connect(const DataSourceConfig& config, CompletionCallback completion) = 0;
    virtual void disconnect(CompletionCallback completion) = 0;
    virtual void reconnect(CompletionCallback completion) = 0;
    virtual void transmit(const ObdRequest& request, CompletionCallback completion) = 0;

    [[nodiscard]] virtual SubscriptionToken subscribe(
        MessageHandler message_handler,
        StateHandler state_handler
    ) = 0;
};

} // namespace revdash::core
