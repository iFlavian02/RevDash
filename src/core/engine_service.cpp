#include "revdash/core/engine_service.hpp"

#include <array>
#include <utility>

#include "revdash/drivers/synthetic.hpp"
#include "revdash/protocol/mode01.hpp"

namespace revdash::core {

struct EngineService::EventSubscriber {
    std::recursive_mutex mutex;
    bool active{true};
    EngineEventHandler handler;
};

namespace {

constexpr std::array kReconnectDelays{
    std::chrono::milliseconds{500}, std::chrono::milliseconds{1000},
    std::chrono::milliseconds{2000}, std::chrono::milliseconds{5000},
    std::chrono::milliseconds{5000}};

Error invalidState(std::string message) {
    return Error{
        .domain = ErrorDomain::Core,
        .code = std::string{toString(ErrorCode::CoreInvalidState)},
        .message = std::move(message),
        .retryable = false,
        .context = {}};
}

} // namespace

EngineService::EngineService()
    : source_to_engine_(std::make_unique<SourceToEngineQueue>()),
      engine_to_recorder_(std::make_unique<EngineToRecorderQueue>()),
      worker_([this](std::stop_token token) { run(token); }),
      recorder_worker_([this](std::stop_token token) { recorderRun(token); }) {
    const auto initial_epoch = epoch_.load(std::memory_order_relaxed);
    telemetry_store_.setEpoch(initial_epoch);
    diagnostic_evaluator_.setEpoch(initial_epoch);
}

EngineService::~EngineService() {
    disconnect();
    worker_.request_stop();
    command_ready_.notify_all();
    worker_.join();
    recorder_worker_.request_stop();
    recorder_worker_.join();
}

void EngineService::enqueue(Command command) {
    {
        std::lock_guard lock(command_mutex_);
        commands_.push_back(std::move(command));
    }
    command_ready_.notify_one();
}

void EngineService::setSource(std::unique_ptr<IDataSource> source, EngineCompletion completion) {
    auto pending_source = std::make_shared<std::unique_ptr<IDataSource>>(std::move(source));
    enqueue([this, pending_source, completion = std::move(completion)]() mutable {
        if (source_ && source_->connectionState() != ConnectionState::Disconnected) {
            source_->disconnect([this, pending_source, completion = std::move(completion)](Result<void>) mutable {
                enqueue([this, pending_source, completion = std::move(completion)]() mutable {
                    source_subscription_.reset(); source_ = std::move(*pending_source); active_config_.reset(); reconnect_at_.reset(); reconnect_attempt_ = 0;
                    invalidateEpoch(); bindSource(); if (completion) completion(makeSuccess());
                });
            });
            return;
        }
        source_subscription_.reset(); source_ = std::move(*pending_source); active_config_.reset(); reconnect_at_.reset(); reconnect_attempt_ = 0;
        invalidateEpoch(); bindSource(); if (completion) completion(makeSuccess());
    });
}

void EngineService::connect(const DataSourceConfig& config, EngineCompletion completion) {
    enqueue([this, config, completion = std::move(completion)]() mutable {
        if (!source_) { if (completion) completion(tl::make_unexpected(invalidState("No active data source"))); return; }
        if (source_->type() != getDataSourceType(config)) { if (completion) completion(tl::make_unexpected(invalidState("Configuration does not match active data source"))); return; }
        active_config_ = config; reconnect_at_.reset(); reconnect_attempt_ = 0; reconnecting_ = false;
        source_->connect(config, [this, completion = std::move(completion)](Result<void> result) mutable {
            enqueue([this, completion = std::move(completion), result = std::move(result)]() mutable {
                if (result) { connection_state_.store(ConnectionState::Ready); publishEvent({.type = EngineEventType::ConnectionStateChanged, .connection_state = ConnectionState::Ready, .epoch = epoch(), .error = std::nullopt}); }
                if (completion) completion(std::move(result));
            });
        });
    });
}

void EngineService::disconnect(EngineCompletion completion) {
    enqueue([this, completion = std::move(completion)]() mutable {
        reconnect_at_.reset(); reconnect_attempt_ = 0; reconnecting_ = false;
        if (!source_) { if (completion) completion(makeSuccess()); return; }
        source_->disconnect([this, completion = std::move(completion)](Result<void> result) mutable {
            enqueue([this, completion = std::move(completion), result = std::move(result)]() mutable {
                connection_state_.store(result ? ConnectionState::Disconnected : ConnectionState::Faulted);
                publishEvent({.type = EngineEventType::ConnectionStateChanged, .connection_state = connectionState(), .epoch = epoch(), .error = std::nullopt});
                if (completion) completion(std::move(result));
            });
        });
    });
}

void EngineService::scan(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { scheduler_.enqueueDiagnostic({.mode = 0x03}); scheduler_.enqueueDiagnostic({.mode = 0x07}); if (completion) completion(makeSuccess()); }); }
void EngineService::identify(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x02}); scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x04}); scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x06}); if (completion) completion(makeSuccess()); }); }
void EngineService::prepareClear(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Clear preparation"); }); }
void EngineService::confirmClear(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Clear confirmation"); }); }
void EngineService::startRecording(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Recording"); }); }
void EngineService::stopRecording(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Recording"); }); }
void EngineService::startPlayback(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Playback"); }); }
void EngineService::stopPlayback(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Playback"); }); }

void EngineService::setSimulationThrottle(double percent, EngineCompletion completion) { enqueue([this, percent, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->setThrottle(percent); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::setSimulationAmbientTemperature(double celsius, EngineCompletion completion) { enqueue([this, celsius, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->setAmbientTemperature(celsius); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::resetSimulation(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->resetSimulation(); invalidateEpoch(); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::setSupportedPids(std::vector<std::uint8_t> pids) { enqueue([this, pids = std::move(pids)]() mutable { scheduler_.setSupportedPids(std::move(pids)); }); }
void EngineService::setOxygenSensorTopology(std::optional<diagnostics::OxygenSensorTopology> topology) { enqueue([this, topology = std::move(topology)]() mutable { diagnostic_evaluator_.setOxygenSensorTopology(std::move(topology)); }); }

TelemetrySnapshot EngineService::telemetrySnapshot() const noexcept { return telemetry_store_.snapshot(); }
std::vector<DiagnosticFinding> EngineService::diagnosticFindings() const { return diagnostic_evaluator_.findings(); }
std::uint64_t EngineService::epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
ConnectionState EngineService::connectionState() const noexcept { return connection_state_.load(std::memory_order_acquire); }
QueueHealth EngineService::sourceQueueHealth() const noexcept { return source_to_engine_->health(); }
QueueHealth EngineService::recorderQueueHealth() const noexcept { return engine_to_recorder_->health(); }

SubscriptionToken EngineService::subscribe(EngineEventHandler handler) {
    auto subscriber = std::make_shared<EventSubscriber>(); subscriber->handler = std::move(handler);
    std::uint64_t id;
    { std::lock_guard lock(event_mutex_); id = ++next_subscriber_id_; subscribers_.emplace_back(id, subscriber); }
    return SubscriptionToken([this, id, weak = std::weak_ptr<EventSubscriber>{subscriber}] {
        if (const auto subscriber = weak.lock()) { std::lock_guard subscriber_lock(subscriber->mutex); subscriber->active = false; }
        std::lock_guard lock(event_mutex_);
        std::erase_if(subscribers_, [id](const auto& item) { return item.first == id; });
    });
}

void EngineService::setRecorderHandler(RecorderHandler handler) { std::lock_guard lock(recorder_mutex_); recorder_handler_ = std::move(handler); }

void EngineService::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        processCommands(); processPackets(); dispatchScheduler();
        if (reconnect_at_ && MonotonicClock::now() >= *reconnect_at_) attemptReconnect();
        std::unique_lock lock(command_mutex_);
        command_ready_.wait_for(lock, stop_token, std::chrono::milliseconds{5}, [this] { return !commands_.empty(); });
    }
    processCommands(); processPackets();
}

void EngineService::recorderRun(std::stop_token stop_token) {
    RecorderPacket packet;
    while (true) {
        if (!engine_to_recorder_->tryPop(packet)) {
            if (stop_token.stop_requested()) break;
            std::this_thread::yield();
            continue;
        }
        RecorderHandler handler; { std::lock_guard lock(recorder_mutex_); handler = recorder_handler_; }
        if (handler) handler(packet);
    }
}

void EngineService::processCommands() {
    std::deque<Command> commands;
    { std::lock_guard lock(command_mutex_); commands.swap(commands_); }
    for (auto& command : commands) command();
}

void EngineService::processPackets() {
    SourceToEnginePacket packet;
    while (source_to_engine_->tryPop(packet)) {
        if (packet.engine_epoch != epoch()) continue;
        const auto payload = packet.message.payload();
        if (payload.size() >= 2 && payload[0] == 0x41) {
            const auto decoded = protocol::decodeMode01Response(packet.message, payload[1]);
            if (!decoded) { publishEvent({.type = EngineEventType::Error, .epoch = epoch(), .error = decoded.error()}); continue; }
            for (const auto& sample : *decoded) { telemetry_store_.update(sample); metric_aggregator_.ingest(sample); diagnostic_evaluator_.ingest(sample); }
            publishEvent({.type = EngineEventType::TelemetryUpdated, .connection_state = connectionState(), .epoch = epoch(), .error = std::nullopt});
            if (diagnostic_evaluator_.evaluate(packet.message.monotonic_ts)) {
                publishEvent({.type = EngineEventType::DiagnosticFindingsUpdated, .connection_state = connectionState(), .epoch = epoch(), .error = std::nullopt});
            }
        }
        static_cast<void>(engine_to_recorder_->tryPush(RecorderPacket{.engine_epoch = packet.engine_epoch, .message = packet.message}));
    }
}

void EngineService::dispatchScheduler() {
    if (!source_ || source_->connectionState() != ConnectionState::Ready) return;
    const auto request = scheduler_.next(MonotonicClock::now());
    if (!request) return;
    const auto started = MonotonicClock::now();
    source_->transmit(*request, [this, started](Result<void> result) {
        enqueue([this, started, result = std::move(result)]() mutable {
            const auto now = MonotonicClock::now(); scheduler_.complete(now, std::chrono::duration_cast<std::chrono::milliseconds>(now - started), !result);
            if (!result) publishEvent({.type = EngineEventType::Error, .connection_state = connectionState(), .epoch = epoch(), .error = result.error()});
        });
    });
}

void EngineService::publishEvent(EngineEvent event) {
    std::vector<std::shared_ptr<EventSubscriber>> subscribers;
    { std::lock_guard lock(event_mutex_); for (const auto& [id, subscriber] : subscribers_) subscribers.push_back(subscriber); }
    for (const auto& subscriber : subscribers) { std::lock_guard lock(subscriber->mutex); if (subscriber->active && subscriber->handler) subscriber->handler(event); }
}

void EngineService::bindSource() {
    if (!source_) return;
    const auto bound_epoch = epoch();
    source_subscription_ = source_->subscribe(
        [this, bound_epoch](const ObdMessage& message) { static_cast<void>(source_to_engine_->tryPush(SourceToEnginePacket{.engine_epoch = bound_epoch, .message = message})); command_ready_.notify_one(); },
        [this](ConnectionState state, const std::optional<Error>& error) { enqueue([this, state, error] { handleSourceState(state, error); }); });
    connection_state_.store(source_->connectionState());
}

void EngineService::invalidateEpoch() {
    const auto new_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    SourceToEnginePacket stale; while (source_to_engine_->tryPop(stale)) {}
    RecorderPacket record; while (engine_to_recorder_->tryPop(record)) {}
    metric_aggregator_.setEpoch(new_epoch); diagnostic_evaluator_.setEpoch(new_epoch); telemetry_store_.setEpoch(new_epoch);
}

void EngineService::handleSourceState(ConnectionState state, const std::optional<Error>& error) {
    connection_state_.store(state); publishEvent({.type = EngineEventType::ConnectionStateChanged, .connection_state = state, .epoch = epoch(), .error = error});
    if (state == ConnectionState::Faulted && error && error->retryable && active_config_) scheduleReconnect();
}

void EngineService::scheduleReconnect() {
    if (reconnect_attempt_ >= kReconnectDelays.size()) return;
    reconnect_at_ = MonotonicClock::now() + kReconnectDelays[reconnect_attempt_++]; reconnecting_ = true;
}

void EngineService::attemptReconnect() {
    reconnect_at_.reset();
    if (!source_ || !active_config_) return;
    source_->reconnect([this](Result<void> result) { enqueue([this, result = std::move(result)]() mutable { if (result) { reconnect_attempt_ = 0; reconnecting_ = false; } else scheduleReconnect(); }); });
}

void EngineService::completeUnsupported(EngineCompletion completion, const char* operation) {
    if (completion) completion(makeError(ErrorCode::DiagnosticsUnsupported, std::string{operation} + " is not available until its dedicated service stage"));
}

} // namespace revdash::core
