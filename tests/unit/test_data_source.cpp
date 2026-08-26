#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "revdash/core/data_source.hpp"

using namespace revdash::core;

namespace {

class FakeDataSource final : public IDataSource {
public:
    explicit FakeDataSource(DataSourceType type = DataSourceType::Synthetic)
        : type_(type), state_(ConnectionState::Disconnected) {}

    ~FakeDataSource() override {
        // Stop callbacks and disarm before destruction
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.clear();
    }

    [[nodiscard]] DataSourceType type() const noexcept override {
        return type_;
    }

    [[nodiscard]] ConnectionState connectionState() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<DataSourceConfig> currentConfig() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    void connect(const DataSourceConfig& config, CompletionCallback completion) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config_ = config;
        }

        setState(ConnectionState::Connecting, std::nullopt);
        setState(ConnectionState::Ready, std::nullopt);

        if (completion) {
            completion(makeSuccess());
        }
    }

    void disconnect(CompletionCallback completion) override {
        const auto prev_state = state_.load(std::memory_order_acquire);
        if (prev_state == ConnectionState::Disconnected) {
            // Idempotent disconnect
            if (completion) {
                completion(makeSuccess());
            }
            return;
        }

        setState(ConnectionState::Disconnecting, std::nullopt);

        // Cancel pending requests with Core.Cancelled
        std::vector<CompletionCallback> pending_to_cancel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!pending_transmits_.empty()) {
                pending_to_cancel.push_back(std::move(pending_transmits_.front()));
                pending_transmits_.pop_front();
            }
        }

        for (auto& cb : pending_to_cancel) {
            cb(makeError(ErrorDomain::Core, "Core.Cancelled", "Operation cancelled due to disconnection", false));
        }

        setState(ConnectionState::Disconnected, std::nullopt);

        if (completion) {
            completion(makeSuccess());
        }
    }

    void reconnect(CompletionCallback completion) override {
        std::optional<DataSourceConfig> cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
        }

        if (!cfg.has_value()) {
            if (completion) {
                completion(makeError(ErrorDomain::Core, "Core.InvalidState", "No previous configuration to reconnect", false));
            }
            return;
        }

        setState(ConnectionState::Reconnecting, std::nullopt);
        connect(*cfg, std::move(completion));
    }

    void transmit([[maybe_unused]] const ObdRequest& request, CompletionCallback completion) override {
        if (state_.load(std::memory_order_acquire) != ConnectionState::Ready) {
            if (completion) {
                completion(makeError(ErrorDomain::Transport, "Transport.NotConnected", "Cannot transmit while not ready", true));
            }
            return;
        }

        // Simulate async queued transmission
        std::lock_guard<std::mutex> lock(mutex_);
        pending_transmits_.push_back(std::move(completion));
    }

    void completeNextTransmit(Result<void> res) {
        CompletionCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_transmits_.empty()) {
                cb = std::move(pending_transmits_.front());
                pending_transmits_.pop_front();
            }
        }
        if (cb) {
            cb(std::move(res));
        }
    }

    void broadcastMessage(const ObdMessage& msg) {
        std::vector<MessageHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, sub] : subscribers_) {
                if (sub.msg_handler) {
                    handlers.push_back(sub.msg_handler);
                }
            }
        }
        for (const auto& h : handlers) {
            h(msg);
        }
    }

    [[nodiscard]] SubscriptionToken subscribe(
        MessageHandler message_handler,
        StateHandler state_handler
    ) override {
        const auto id = ++next_subscriber_id_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            subscribers_[id] = Subscriber{
                .msg_handler = std::move(message_handler),
                .state_handler = std::move(state_handler)
            };
        }

        return SubscriptionToken([this, id]() {
            std::lock_guard<std::mutex> lock(mutex_);
            subscribers_.erase(id);
        });
    }

    [[nodiscard]] std::size_t subscriberCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return subscribers_.size();
    }

private:
    struct Subscriber {
        MessageHandler msg_handler;
        StateHandler state_handler;
    };

    void setState(ConnectionState state, const std::optional<Error>& err) {
        state_.store(state, std::memory_order_release);
        std::vector<StateHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, sub] : subscribers_) {
                if (sub.state_handler) {
                    handlers.push_back(sub.state_handler);
                }
            }
        }
        for (const auto& h : handlers) {
            h(state, err);
        }
    }

    DataSourceType type_;
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    mutable std::mutex mutex_;
    std::optional<DataSourceConfig> config_{std::nullopt};
    std::uint64_t next_subscriber_id_{0};
    std::unordered_map<std::uint64_t, Subscriber> subscribers_;
    std::deque<CompletionCallback> pending_transmits_;
};

} // namespace

TEST_CASE("DataSourceConfig variants and type reflection", "[data_source_contract]") {
    SerialConfig serial{.port_name = "COM4", .baud_rate = 115200};
    DataSourceConfig cfg1 = serial;
    REQUIRE(getDataSourceType(cfg1) == DataSourceType::SerialElm327);

    SyntheticConfig synthetic{.deterministic_seed = 42};
    DataSourceConfig cfg2 = synthetic;
    REQUIRE(getDataSourceType(cfg2) == DataSourceType::Synthetic);

    PlaybackConfig playback{.session_file_path = "run1.jsonl", .speed_multiplier = 2.0};
    DataSourceConfig cfg3 = playback;
    REQUIRE(getDataSourceType(cfg3) == DataSourceType::Playback);

    SocketCanConfig socketcan{.interface_name = "can0", .bitrate = 500000};
    DataSourceConfig cfg4 = socketcan;
    REQUIRE(getDataSourceType(cfg4) == DataSourceType::SocketCan);
}

TEST_CASE("SubscriptionToken RAII lifecycle and move semantics", "[data_source_contract]") {
    SECTION("Default token is inactive") {
        SubscriptionToken token;
        REQUIRE_FALSE(token.active());
        REQUIRE_FALSE(static_cast<bool>(token));
    }

    SECTION("Active token invokes callback on reset and destruction") {
        int unsub_count = 0;
        {
            SubscriptionToken token([&]() { ++unsub_count; });
            REQUIRE(token.active());
            REQUIRE(static_cast<bool>(token));
            REQUIRE(unsub_count == 0);
        }
        REQUIRE(unsub_count == 1);
    }

    SECTION("Multiple resets are idempotent") {
        int unsub_count = 0;
        SubscriptionToken token([&]() { ++unsub_count; });
        token.reset();
        REQUIRE_FALSE(token.active());
        token.reset();
        REQUIRE(unsub_count == 1);
    }

    SECTION("Move construction transfers ownership") {
        int unsub_count = 0;
        {
            SubscriptionToken token1([&]() { ++unsub_count; });
            SubscriptionToken token2(std::move(token1));

            REQUIRE_FALSE(token1.active());
            REQUIRE(token2.active());
            REQUIRE(unsub_count == 0);
        }
        REQUIRE(unsub_count == 1);
    }

    SECTION("Move assignment resets existing target and transfers ownership") {
        int unsub1 = 0;
        int unsub2 = 0;
        {
            SubscriptionToken token1([&]() { ++unsub1; });
            SubscriptionToken token2([&]() { ++unsub2; });

            token1 = std::move(token2);
            REQUIRE(unsub1 == 1);
            REQUIRE(unsub2 == 0);
            REQUIRE(token1.active());
            REQUIRE_FALSE(token2.active());
        }
        REQUIRE(unsub2 == 1);
    }
}

TEST_CASE("IDataSource async lifecycle, reconnect, and idempotent disconnect", "[data_source_contract]") {
    FakeDataSource source(DataSourceType::Synthetic);
    REQUIRE(source.type() == DataSourceType::Synthetic);
    REQUIRE(source.connectionState() == ConnectionState::Disconnected);

    SyntheticConfig cfg{.deterministic_seed = 999};

    SECTION("Connect transitions to Ready") {
        bool connected = false;
        source.connect(cfg, [&](Result<void> res) {
            REQUIRE(res.has_value());
            connected = true;
        });

        REQUIRE(connected);
        REQUIRE(source.connectionState() == ConnectionState::Ready);
        REQUIRE(source.currentConfig().has_value());
    }

    SECTION("Disconnect is idempotent") {
        source.connect(cfg, nullptr);
        REQUIRE(source.connectionState() == ConnectionState::Ready);

        bool disconnected1 = false;
        source.disconnect([&](Result<void> res) {
            REQUIRE(res.has_value());
            disconnected1 = true;
        });
        REQUIRE(disconnected1);
        REQUIRE(source.connectionState() == ConnectionState::Disconnected);

        // Second disconnect when already disconnected
        bool disconnected2 = false;
        source.disconnect([&](Result<void> res) {
            REQUIRE(res.has_value());
            disconnected2 = true;
        });
        REQUIRE(disconnected2);
        REQUIRE(source.connectionState() == ConnectionState::Disconnected);
    }

    SECTION("Reconnect preserves previous configuration") {
        source.connect(cfg, nullptr);
        source.disconnect(nullptr);

        bool reconnected = false;
        source.reconnect([&](Result<void> res) {
            REQUIRE(res.has_value());
            reconnected = true;
        });

        REQUIRE(reconnected);
        REQUIRE(source.connectionState() == ConnectionState::Ready);
        REQUIRE(source.currentConfig().has_value());
        const auto stored = std::get<SyntheticConfig>(*source.currentConfig());
        REQUIRE(stored.deterministic_seed == 999);
    }

    SECTION("Disconnect cancels pending transmissions with Core.Cancelled") {
        source.connect(cfg, nullptr);

        ObdRequest req{.mode = 0x01, .pid = 0x0C};
        bool transmit_cancelled = false;
        source.transmit(req, [&](Result<void> res) {
            REQUIRE_FALSE(res.has_value());
            REQUIRE(res.error().domain == ErrorDomain::Core);
            REQUIRE(res.error().code == "Core.Cancelled");
            transmit_cancelled = true;
        });

        source.disconnect(nullptr);
        REQUIRE(transmit_cancelled);
    }
}

TEST_CASE("IDataSource subscriber isolation and unsubscription", "[data_source_contract]") {
    FakeDataSource source(DataSourceType::Synthetic);

    int msg_count1 = 0;
    int msg_count2 = 0;

    auto token1 = source.subscribe(
        [&](const ObdMessage&) { ++msg_count1; },
        nullptr
    );

    auto token2 = source.subscribe(
        [&](const ObdMessage&) { ++msg_count2; },
        nullptr
    );

    REQUIRE(source.subscriberCount() == 2);

    auto msg = ObdMessage::create(DataSourceType::Synthetic, 0x7E8, std::vector<std::uint8_t>{0x41, 0x0C, 0x1A, 0xF8}).value();
    source.broadcastMessage(msg);

    REQUIRE(msg_count1 == 1);
    REQUIRE(msg_count2 == 1);

    // Reset token1 -> only token2 receives next broadcast
    token1.reset();
    REQUIRE(source.subscriberCount() == 1);

    source.broadcastMessage(msg);
    REQUIRE(msg_count1 == 1);
    REQUIRE(msg_count2 == 2);
}
