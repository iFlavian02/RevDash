#include "revdash/core/async_data_source.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/asio.hpp>

namespace revdash::core {

struct AsyncDataSource::State {
    struct Subscriber {
        std::recursive_mutex mutex;
        bool active{true};
        MessageHandler message_handler;
        StateHandler state_handler;
    };

    explicit State(DataSourceType source_type)
        : type(source_type) {}

    DataSourceType type;
    std::atomic<ConnectionState> connection_state{ConnectionState::Disconnected};
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    std::optional<DataSourceConfig> config;
    std::uint64_t next_operation_id{0};
    std::unordered_map<std::uint64_t, CompletionCallback> pending_operations;
    std::uint64_t next_subscriber_id{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<Subscriber>> subscribers;
};

class AsyncDataSource::Impl {
public:
    Impl()
        : work_guard(boost::asio::make_work_guard(io_context)),
          worker([this](std::stop_token) { io_context.run(); }) {}

    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    std::jthread worker;

    void cancelDelayedOperations() {
        std::vector<std::shared_ptr<boost::asio::steady_timer>> timers_to_cancel;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            timers_to_cancel.swap(delayed_timers);
        }
        for (const auto& timer : timers_to_cancel) {
            timer->cancel();
        }
    }

    std::mutex timer_mutex;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> delayed_timers;
};

namespace {

Error cancelledError() {
    return Error{
        .domain = ErrorDomain::Core,
        .code = std::string{toString(ErrorCode::CoreCancelled)},
        .message = "Operation cancelled during source shutdown or replacement",
        .retryable = false
    };
}

Error invalidStateError(std::string message) {
    return Error{
        .domain = ErrorDomain::Core,
        .code = std::string{toString(ErrorCode::CoreInvalidState)},
        .message = std::move(message),
        .retryable = false
    };
}

Error notConnectedError() {
    return Error{
        .domain = ErrorDomain::Transport,
        .code = std::string{toString(ErrorCode::TransportNotConnected)},
        .message = "Cannot transmit while the source is not ready",
        .retryable = true
    };
}

} // namespace

AsyncDataSource::AsyncDataSource(DataSourceType type)
    : type_(type), state_(std::make_shared<State>(type)), impl_(std::make_unique<Impl>()) {}

AsyncDataSource::~AsyncDataSource() {
    state_->stopping.store(true, std::memory_order_release);
    impl_->cancelDelayedOperations();

    std::promise<void> shutdown_complete;
    auto shutdown_ready = shutdown_complete.get_future();
    boost::asio::post(impl_->io_context, [state = state_, completion = std::move(shutdown_complete)]() mutable {
        std::unordered_map<std::uint64_t, CompletionCallback> pending;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            pending.swap(state->pending_operations);
            state->subscribers.clear();
            state->connection_state.store(ConnectionState::Disconnected, std::memory_order_release);
        }
        for (auto& [id, callback] : pending) {
            if (callback) {
                callback(tl::make_unexpected(cancelledError()));
            }
        }
        completion.set_value();
    });
    shutdown_ready.wait();
    impl_->work_guard.reset();
    impl_->worker.join();
}

DataSourceType AsyncDataSource::type() const noexcept {
    return type_;
}

ConnectionState AsyncDataSource::connectionState() const noexcept {
    return state_->connection_state.load(std::memory_order_acquire);
}

std::optional<DataSourceConfig> AsyncDataSource::currentConfig() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->config;
}

void AsyncDataSource::postToWorker(std::function<void()> operation) {
    boost::asio::post(impl_->io_context, std::move(operation));
}

void AsyncDataSource::postAfterToWorker(std::chrono::milliseconds delay, std::function<void()> operation) {
    if (delay <= std::chrono::milliseconds::zero()) {
        postToWorker(std::move(operation));
        return;
    }
    auto timer = std::make_shared<boost::asio::steady_timer>(impl_->io_context, delay);
    {
        std::lock_guard<std::mutex> lock(impl_->timer_mutex);
        impl_->delayed_timers.push_back(timer);
    }
    timer->async_wait([this, timer, operation = std::move(operation)](const boost::system::error_code& error) mutable {
        {
            std::lock_guard<std::mutex> lock(impl_->timer_mutex);
            const auto found = std::find(impl_->delayed_timers.begin(), impl_->delayed_timers.end(), timer);
            if (found != impl_->delayed_timers.end()) {
                impl_->delayed_timers.erase(found);
            }
        }
        if (!error && operation) {
            operation();
        }
    });
}

void AsyncDataSource::connect(const DataSourceConfig& config, CompletionCallback completion) {
    postToWorker([this, config, completion = std::move(completion)]() mutable {
        if (state_->stopping.load(std::memory_order_acquire)) {
            if (completion) {
                completion(tl::make_unexpected(cancelledError()));
            }
            return;
        }
        if (getDataSourceType(config) != type_) {
            if (completion) {
                completion(tl::make_unexpected(invalidStateError("Configuration does not match this data source type")));
            }
            return;
        }

        transition(ConnectionState::Connecting);
        std::uint64_t operation_id;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->config = config;
            operation_id = ++state_->next_operation_id;
            state_->pending_operations.emplace(operation_id, std::move(completion));
        }
        startConnect(config, [this, operation_id](Result<void> result) {
            postToWorker([this, operation_id, result = std::move(result)]() mutable {
                completeConnect(operation_id, std::move(result));
            });
        });
    });
}

void AsyncDataSource::disconnect(CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        if (state_->stopping.load(std::memory_order_acquire)) {
            if (completion) {
                completion(tl::make_unexpected(cancelledError()));
            }
            return;
        }
        if (connectionState() == ConnectionState::Disconnected) {
            if (completion) {
                completion(makeSuccess());
            }
            return;
        }

        transition(ConnectionState::Disconnecting);
        std::unordered_map<std::uint64_t, CompletionCallback> pending;
        std::uint64_t operation_id;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            pending.swap(state_->pending_operations);
            operation_id = ++state_->next_operation_id;
            state_->pending_operations.emplace(operation_id, std::move(completion));
        }
        for (auto& [id, callback] : pending) {
            if (id != operation_id && callback) {
                callback(tl::make_unexpected(cancelledError()));
            }
        }
        startDisconnect([this, operation_id](Result<void> result) {
            postToWorker([this, operation_id, result = std::move(result)]() mutable {
                completeDisconnect(operation_id, std::move(result));
            });
        });
    });
}

void AsyncDataSource::reconnect(CompletionCallback completion) {
    postToWorker([this, completion = std::move(completion)]() mutable {
        if (state_->stopping.load(std::memory_order_acquire)) {
            if (completion) {
                completion(tl::make_unexpected(cancelledError()));
            }
            return;
        }
        std::optional<DataSourceConfig> config;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            config = state_->config;
        }
        if (!config.has_value()) {
            if (completion) {
                completion(tl::make_unexpected(invalidStateError("No previous configuration is available for reconnect")));
            }
            return;
        }

        transition(ConnectionState::Reconnecting);
        std::uint64_t operation_id;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            operation_id = ++state_->next_operation_id;
            state_->pending_operations.emplace(operation_id, std::move(completion));
        }
        startConnect(*config, [this, operation_id](Result<void> result) {
            postToWorker([this, operation_id, result = std::move(result)]() mutable {
                completeConnect(operation_id, std::move(result));
            });
        });
    });
}

void AsyncDataSource::transmit(const ObdRequest& request, CompletionCallback completion) {
    postToWorker([this, request, completion = std::move(completion)]() mutable {
        if (state_->stopping.load(std::memory_order_acquire)) {
            if (completion) {
                completion(tl::make_unexpected(cancelledError()));
            }
            return;
        }
        if (connectionState() != ConnectionState::Ready) {
            if (completion) {
                completion(tl::make_unexpected(notConnectedError()));
            }
            return;
        }

        std::uint64_t operation_id;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            operation_id = ++state_->next_operation_id;
            state_->pending_operations.emplace(operation_id, std::move(completion));
        }
        startTransmit(request, [this, operation_id](Result<void> result) {
            postToWorker([this, operation_id, result = std::move(result)]() mutable {
                completeTransmit(operation_id, std::move(result));
            });
        });
    });
}

SubscriptionToken AsyncDataSource::subscribe(MessageHandler message_handler, StateHandler state_handler) {
    auto subscriber = std::make_shared<State::Subscriber>();
    subscriber->message_handler = std::move(message_handler);
    subscriber->state_handler = std::move(state_handler);
    std::uint64_t subscriber_id;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        subscriber_id = ++state_->next_subscriber_id;
        state_->subscribers.emplace(subscriber_id, subscriber);
    }
    std::weak_ptr<State> weak_state = state_;
    return SubscriptionToken([weak_state, subscriber_id, subscriber]() {
        std::lock_guard<std::recursive_mutex> subscriber_lock(subscriber->mutex);
        subscriber->active = false;
        if (const auto state = weak_state.lock()) {
            std::lock_guard<std::mutex> state_lock(state->mutex);
            state->subscribers.erase(subscriber_id);
        }
    });
}

void AsyncDataSource::publishMessage(const ObdMessage& message) {
    std::vector<std::shared_ptr<State::Subscriber>> subscribers;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [id, subscriber] : state_->subscribers) {
            subscribers.push_back(subscriber);
        }
    }
    for (const auto& subscriber : subscribers) {
        std::lock_guard<std::recursive_mutex> lock(subscriber->mutex);
        if (subscriber->active && subscriber->message_handler) {
            subscriber->message_handler(message);
        }
    }
}

void AsyncDataSource::completeConnect(std::uint64_t operation_id, Result<void> result) {
    CompletionCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->pending_operations.find(operation_id);
        if (found == state_->pending_operations.end()) {
            return;
        }
        callback = std::move(found->second);
        state_->pending_operations.erase(found);
    }
    if (result.has_value()) {
        transition(ConnectionState::Ready);
    } else {
        transition(ConnectionState::Faulted, result.error());
    }
    if (callback) {
        callback(std::move(result));
    }
}

void AsyncDataSource::completeDisconnect(std::uint64_t operation_id, Result<void> result) {
    CompletionCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->pending_operations.find(operation_id);
        if (found == state_->pending_operations.end()) {
            return;
        }
        callback = std::move(found->second);
        state_->pending_operations.erase(found);
    }
    transition(result.has_value() ? ConnectionState::Disconnected : ConnectionState::Faulted,
               result.has_value() ? std::nullopt : std::optional<Error>{result.error()});
    if (callback) {
        callback(std::move(result));
    }
}

void AsyncDataSource::completeTransmit(std::uint64_t operation_id, Result<void> result) {
    CompletionCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->pending_operations.find(operation_id);
        if (found == state_->pending_operations.end()) {
            return;
        }
        callback = std::move(found->second);
        state_->pending_operations.erase(found);
    }
    if (callback) {
        callback(std::move(result));
    }
}

void AsyncDataSource::transition(ConnectionState connection_state, const std::optional<Error>& error) {
    state_->connection_state.store(connection_state, std::memory_order_release);
    std::vector<std::shared_ptr<State::Subscriber>> subscribers;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [id, subscriber] : state_->subscribers) {
            subscribers.push_back(subscriber);
        }
    }
    for (const auto& subscriber : subscribers) {
        std::lock_guard<std::recursive_mutex> lock(subscriber->mutex);
        if (subscriber->active && subscriber->state_handler) {
            subscriber->state_handler(connection_state, error);
        }
    }
}

} // namespace revdash::core
