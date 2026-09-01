#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include "revdash/core/data_source.hpp"

namespace revdash::core {

class AsyncDataSource : public IDataSource {
public:
    explicit AsyncDataSource(DataSourceType type);
    ~AsyncDataSource() override;

    AsyncDataSource(const AsyncDataSource&) = delete;
    AsyncDataSource& operator=(const AsyncDataSource&) = delete;

    [[nodiscard]] DataSourceType type() const noexcept final;
    [[nodiscard]] ConnectionState connectionState() const noexcept final;
    [[nodiscard]] std::optional<DataSourceConfig> currentConfig() const noexcept final;

    void connect(const DataSourceConfig& config, CompletionCallback completion) final;
    void disconnect(CompletionCallback completion) final;
    void reconnect(CompletionCallback completion) final;
    void transmit(const ObdRequest& request, CompletionCallback completion) final;

    [[nodiscard]] SubscriptionToken subscribe(MessageHandler message_handler, StateHandler state_handler) final;

protected:
    virtual void startConnect(const DataSourceConfig& config, CompletionCallback completion) = 0;
    virtual void startDisconnect(CompletionCallback completion) = 0;
    virtual void startTransmit(const ObdRequest& request, CompletionCallback completion) = 0;

    void postToWorker(std::function<void()> operation);
    void postAfterToWorker(std::chrono::milliseconds delay, std::function<void()> operation);
    void publishMessage(const ObdMessage& message);

private:
    struct State;

    void completeConnect(std::uint64_t operation_id, Result<void> result);
    void completeDisconnect(std::uint64_t operation_id, Result<void> result);
    void completeTransmit(std::uint64_t operation_id, Result<void> result);
    void transition(ConnectionState state, const std::optional<Error>& error = std::nullopt);

    DataSourceType type_;
    std::shared_ptr<State> state_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace revdash::core
