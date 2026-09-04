#include "revdash/core/engine_service.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

#include "revdash/drivers/synthetic.hpp"
#include "revdash/protocol/diagnostics.hpp"
#include "revdash/protocol/mode01.hpp"

namespace revdash::core {

struct EngineService::EventSubscriber {
    std::recursive_mutex mutex;
    bool active{true};
    EngineEventHandler handler;
};

struct EngineService::DiagnosticOperation {
    enum class Kind { Scan, Identify, Clear, PostClearScan };

    Kind kind{Kind::Scan};
    std::uint64_t epoch{0};
    std::size_t outstanding_requests{0};
    std::vector<DtcRecord> dtcs{};
    std::vector<EcuMetadata> metadata{};
    std::optional<std::string> freeze_frame_dtc{std::nullopt};
    bool freeze_frame_requested{false};
    bool positive_clear_response{false};
    std::optional<Error> error{std::nullopt};
    DiagnosticScanCompletion scan_completion{};
    IdentificationCompletion identification_completion{};
    ClearConfirmationCompletion clear_completion{};
    std::optional<Mode04AuditRecord> audit{std::nullopt};
};

struct EngineService::ClearTokenState {
    ClearDtcPreparation preparation{};
    MonotonicTimePoint prepared_at{MonotonicClock::now()};
    DataSourceType source_type{DataSourceType::SerialElm327};
    DataSourceConfig source_config{SerialConfig{}};
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

Error diagnosticError(ErrorCode code, std::string message) {
    return Error{
        .domain = errorDomain(code),
        .code = std::string{toString(code)},
        .message = std::move(message),
        .retryable = false,
        .context = {}};
}

std::string makeConfirmationToken() {
    std::random_device random;
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index) {
        stream << std::setw(8) << random();
    }
    return stream.str();
}

} // namespace

EngineService::EngineService(std::shared_ptr<IClock> clock, std::chrono::milliseconds post_clear_settling_delay)
    : source_to_engine_(std::make_unique<SourceToEngineQueue>()),
      engine_to_recorder_(std::make_unique<EngineToRecorderQueue>()),
      clock_(clock ? std::move(clock) : std::make_shared<SystemClockSource>()),
      post_clear_settling_delay_(post_clear_settling_delay),
      worker_([this](std::stop_token token) { run(token); }),
      recorder_worker_([this](std::stop_token token) { recorderRun(token); }) {
    const auto initial_epoch = epoch_.load(std::memory_order_relaxed);
    telemetry_store_.setEpoch(initial_epoch);
    diagnostic_evaluator_.setEpoch(initial_epoch);
    diagnostic_snapshot_.engine_epoch = initial_epoch;
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
        invalidateClearPreparation();
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
        invalidateClearPreparation(); active_config_ = config; reconnect_at_.reset(); reconnect_attempt_ = 0; reconnecting_ = false;
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
        invalidateClearPreparation();
        cancelDiagnosticOperation(Error{
            .domain = ErrorDomain::Core,
            .code = std::string{toString(ErrorCode::CoreCancelled)},
            .message = "Diagnostic operation was cancelled by disconnect",
            .retryable = false,
            .context = {}});
        scheduler_.reset();
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

void EngineService::scan(DiagnosticScanCompletion completion) {
    enqueue([this, completion = std::move(completion)]() mutable { beginScan(std::move(completion)); });
}

void EngineService::identify(IdentificationCompletion completion) {
    enqueue([this, completion = std::move(completion)]() mutable {
        invalidateClearPreparation();
        if (diagnostic_operation_) {
            if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsOperationInProgress, "Another diagnostic operation is active")));
            return;
        }
        if (!source_ || source_->connectionState() != ConnectionState::Ready) {
            if (completion) completion(tl::make_unexpected(invalidState("Identification requires a ready data source")));
            return;
        }
        diagnostic_operation_ = std::make_unique<DiagnosticOperation>();
        diagnostic_operation_->kind = DiagnosticOperation::Kind::Identify;
        diagnostic_operation_->epoch = epoch();
        diagnostic_operation_->outstanding_requests = 3;
        diagnostic_operation_->identification_completion = std::move(completion);
        scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x02});
        scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x04});
        scheduler_.enqueueDiagnostic({.mode = 0x09, .pid = 0x06});
    });
}

void EngineService::prepareClear(ClearPreparationCompletion completion) {
    enqueue([this, completion = std::move(completion)]() mutable {
        invalidateClearPreparation();
        if (const auto safety = validateClearSafety(); !safety) {
            if (completion) completion(tl::make_unexpected(safety.error()));
            return;
        }

        DiagnosticSnapshot snapshot;
        {
            std::lock_guard lock(diagnostic_mutex_);
            snapshot = diagnostic_snapshot_;
        }
        snapshot.findings = diagnostic_evaluator_.findings();
        snapshot.captured_at = clock_->monotonicNow();
        snapshot.engine_epoch = epoch();

        ClearDtcPreparation preparation{
            .confirmation_token = makeConfirmationToken(),
            .expires_at = clock_->monotonicNow() + std::chrono::seconds{30},
            .snapshot = std::move(snapshot),
            .vehicle_identity = currentVehicleIdentity(),
            .warning = std::string{kClearDiagnosticWarning}};
        clear_token_ = std::make_unique<ClearTokenState>(ClearTokenState{
            .preparation = preparation,
            .prepared_at = clock_->monotonicNow(),
            .source_type = source_->type(),
            .source_config = *active_config_});
        if (completion) completion(std::move(preparation));
    });
}

void EngineService::confirmClear(std::string confirmation_token, ClearConfirmationCompletion completion) {
    enqueue([this, confirmation_token = std::move(confirmation_token), completion = std::move(completion)]() mutable {
        if (!clear_token_) {
            if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsTokenInvalid, "No active clear confirmation exists")));
            return;
        }
        if (clear_token_->preparation.confirmation_token != confirmation_token) {
            if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsTokenInvalid, "The clear confirmation token does not match")));
            return;
        }
        if (clock_->monotonicNow() >= clear_token_->preparation.expires_at) {
            invalidateClearPreparation();
            if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsTokenExpired, "The clear confirmation token has expired")));
            return;
        }

        auto token = std::move(clear_token_); // Atomic consumption on the engine worker.
        if (token->preparation.snapshot.engine_epoch != epoch() || !source_ || !active_config_ ||
            token->source_type != source_->type() || token->source_config != *active_config_ ||
            token->preparation.vehicle_identity != currentVehicleIdentity()) {
            if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsTokenInvalid, "The source, vehicle, or engine epoch changed after clear preparation")));
            return;
        }
        if (const auto safety = validateClearSafety(); !safety) {
            if (completion) completion(tl::make_unexpected(safety.error()));
            return;
        }

        diagnostic_operation_ = std::make_unique<DiagnosticOperation>();
        diagnostic_operation_->kind = DiagnosticOperation::Kind::Clear;
        diagnostic_operation_->epoch = epoch();
        diagnostic_operation_->outstanding_requests = 1;
        diagnostic_operation_->clear_completion = std::move(completion);
        diagnostic_operation_->audit = Mode04AuditRecord{
            .prepared_at = token->prepared_at,
            .completed_at = clock_->monotonicNow(),
            .completed_utc = clock_->utcNow(),
            .source_type = token->source_type,
            .engine_epoch = epoch(),
            .vehicle_identity = token->preparation.vehicle_identity,
            .warning = token->preparation.warning,
            .preparation_snapshot = token->preparation.snapshot,
            .post_clear_dtcs = {},
            .request_transmitted = false,
            .positive_response = false,
            .post_clear_rescan_completed = false,
            .error = std::nullopt};
        scheduler_.enqueueDiagnostic(protocol::makeClearDiagnosticRequest());
    });
}
void EngineService::startRecording(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Recording"); }); }
void EngineService::stopRecording(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Recording"); }); }
void EngineService::startPlayback(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Playback"); }); }
void EngineService::stopPlayback(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { completeUnsupported(std::move(completion), "Playback"); }); }

void EngineService::setSimulationThrottle(double percent, EngineCompletion completion) { enqueue([this, percent, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->setThrottle(percent); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::setSimulationAmbientTemperature(double celsius, EngineCompletion completion) { enqueue([this, celsius, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->setAmbientTemperature(celsius); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::resetSimulation(EngineCompletion completion) { enqueue([this, completion = std::move(completion)]() mutable { if (auto* source = dynamic_cast<drivers::SyntheticDataSource*>(source_.get())) { source->resetSimulation(); invalidateEpoch(); if (completion) completion(makeSuccess()); } else if (completion) completion(tl::make_unexpected(invalidState("Simulation controls require the synthetic source"))); }); }
void EngineService::setSupportedPids(std::vector<std::uint8_t> pids) { enqueue([this, pids = std::move(pids)]() mutable { scheduler_.setSupportedPids(std::move(pids)); }); }
void EngineService::setOxygenSensorTopology(std::optional<diagnostics::OxygenSensorTopology> topology) { enqueue([this, topology = std::move(topology)]() mutable { diagnostic_evaluator_.setOxygenSensorTopology(std::move(topology)); }); }
void EngineService::setDtcDatabase(std::shared_ptr<const diagnostics::DtcDatabase> database) { enqueue([this, database = std::move(database)]() mutable { dtc_database_ = std::move(database); }); }

TelemetrySnapshot EngineService::telemetrySnapshot() const noexcept { return telemetry_store_.snapshot(); }
std::vector<DiagnosticFinding> EngineService::diagnosticFindings() const { return diagnostic_evaluator_.findings(); }
DiagnosticSnapshot EngineService::diagnosticSnapshot() const { std::lock_guard lock(diagnostic_mutex_); return diagnostic_snapshot_; }
std::vector<EcuMetadata> EngineService::ecuMetadata() const { std::lock_guard lock(diagnostic_mutex_); return ecu_metadata_; }
std::vector<Mode04AuditRecord> EngineService::mode04AuditRecords() const { std::lock_guard lock(diagnostic_mutex_); return mode04_audits_; }
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
        if (clear_settle_at_ && clock_->monotonicNow() >= *clear_settle_at_) {
            clear_settle_at_.reset();
            if (diagnostic_operation_ && diagnostic_operation_->kind == DiagnosticOperation::Kind::Clear) {
                diagnostic_operation_->kind = DiagnosticOperation::Kind::PostClearScan;
                diagnostic_operation_->outstanding_requests = 2;
                diagnostic_operation_->dtcs.clear();
                scheduler_.enqueueDiagnostic({.mode = 0x03});
                scheduler_.enqueueDiagnostic({.mode = 0x07});
            }
        }
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
        handleDiagnosticMessage(packet.message);
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
    const auto dispatch_epoch = epoch();
    source_->transmit(*request, [this, started, request = *request, dispatch_epoch](Result<void> result) {
        enqueue([this, started, request, dispatch_epoch, result = std::move(result)]() mutable {
            if (dispatch_epoch != epoch()) return;
            processPackets();
            const auto now = MonotonicClock::now(); scheduler_.complete(now, std::chrono::duration_cast<std::chrono::milliseconds>(now - started), !result);
            if (!result) publishEvent({.type = EngineEventType::Error, .connection_state = connectionState(), .epoch = epoch(), .error = result.error()});
            finishDiagnosticRequest(request, std::move(result));
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
    invalidateClearPreparation();
    cancelDiagnosticOperation(Error{
        .domain = ErrorDomain::Core,
        .code = std::string{toString(ErrorCode::CoreCancelled)},
        .message = "Diagnostic operation was cancelled by an engine epoch change",
        .retryable = false,
        .context = {}});
    clear_settle_at_.reset();
    scheduler_.reset();
    const auto new_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    SourceToEnginePacket stale; while (source_to_engine_->tryPop(stale)) {}
    RecorderPacket record; while (engine_to_recorder_->tryPop(record)) {}
    metric_aggregator_.setEpoch(new_epoch); diagnostic_evaluator_.setEpoch(new_epoch); telemetry_store_.setEpoch(new_epoch);
}

void EngineService::handleSourceState(ConnectionState state, const std::optional<Error>& error) {
    if (state != ConnectionState::Ready) {
        invalidateClearPreparation();
        if (state == ConnectionState::Faulted || state == ConnectionState::Disconnected) {
            cancelDiagnosticOperation(error.value_or(Error{
                .domain = ErrorDomain::Core,
                .code = std::string{toString(ErrorCode::CoreCancelled)},
                .message = "Diagnostic operation was cancelled because the source became unavailable",
                .retryable = false,
                .context = {}}));
        }
    }
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

void EngineService::beginScan(DiagnosticScanCompletion completion, bool post_clear_rescan) {
    if (!post_clear_rescan) invalidateClearPreparation();
    if (diagnostic_operation_) {
        if (completion) completion(tl::make_unexpected(diagnosticError(ErrorCode::DiagnosticsOperationInProgress, "Another diagnostic operation is active")));
        return;
    }
    if (!source_ || source_->connectionState() != ConnectionState::Ready) {
        if (completion) completion(tl::make_unexpected(invalidState("A diagnostic scan requires a ready data source")));
        return;
    }
    diagnostic_operation_ = std::make_unique<DiagnosticOperation>();
    diagnostic_operation_->kind = post_clear_rescan ? DiagnosticOperation::Kind::PostClearScan : DiagnosticOperation::Kind::Scan;
    diagnostic_operation_->epoch = epoch();
    diagnostic_operation_->outstanding_requests = 2;
    diagnostic_operation_->scan_completion = std::move(completion);
    scheduler_.enqueueDiagnostic({.mode = 0x03});
    scheduler_.enqueueDiagnostic({.mode = 0x07});
}

void EngineService::finishDiagnosticRequest(const ObdRequest& request, Result<void> result) {
    if (!diagnostic_operation_ || diagnostic_operation_->epoch != epoch()) return;

    const auto kind = diagnostic_operation_->kind;
    const bool belongs =
        ((kind == DiagnosticOperation::Kind::Scan || kind == DiagnosticOperation::Kind::PostClearScan) &&
         (request.mode == 0x02 || request.mode == 0x03 || request.mode == 0x07)) ||
        (kind == DiagnosticOperation::Kind::Identify && request.mode == 0x09) ||
        (kind == DiagnosticOperation::Kind::Clear && request.mode == 0x04);
    if (!belongs) return;

    if (!result && !diagnostic_operation_->error) diagnostic_operation_->error = result.error();
    if (kind == DiagnosticOperation::Kind::Clear && result && diagnostic_operation_->audit) {
        diagnostic_operation_->audit->request_transmitted = true;
    }
    if (diagnostic_operation_->outstanding_requests > 0) --diagnostic_operation_->outstanding_requests;
    if (diagnostic_operation_->outstanding_requests != 0) return;

    if (kind == DiagnosticOperation::Kind::Clear) {
        if (diagnostic_operation_->error) {
            finishClear(diagnostic_operation_->error);
        } else if (!diagnostic_operation_->positive_clear_response) {
            finishClear(diagnosticError(ErrorCode::ProtocolMalformedResponse, "Mode 04 completed without a positive ECU response"));
        } else {
            if (diagnostic_operation_->audit) {
                diagnostic_operation_->audit->positive_response = true;
            }
            clear_settle_at_ = clock_->monotonicNow() + post_clear_settling_delay_;
        }
        return;
    }

    if (kind == DiagnosticOperation::Kind::Scan || kind == DiagnosticOperation::Kind::PostClearScan) {
        if (!diagnostic_operation_->freeze_frame_requested && !diagnostic_operation_->dtcs.empty()) {
            diagnostic_operation_->freeze_frame_requested = true;
            diagnostic_operation_->freeze_frame_dtc = diagnostic_operation_->dtcs.front().code;
            diagnostic_operation_->outstanding_requests = 1;
            scheduler_.enqueueDiagnostic({.mode = 0x02, .pid = 0x0C});
            return;
        }
        finishScan();
        return;
    }
    finishIdentification();
}

void EngineService::handleDiagnosticMessage(const ObdMessage& message) {
    if (!diagnostic_operation_ || diagnostic_operation_->epoch != epoch()) return;
    const auto payload = message.payload();
    if (payload.empty()) return;

    if (payload[0] == 0x7F) {
        if (diagnostic_operation_->kind == DiagnosticOperation::Kind::Clear && payload.size() >= 2 && payload[1] == 0x04) {
            diagnostic_operation_->error = diagnosticError(ErrorCode::ProtocolNegativeResponse, "The ECU rejected the Mode 04 request");
        }
        return; // Unsupported services are tolerated for scan/identification partial support.
    }

    auto remember_error = [this](const Error& error) {
        if (!diagnostic_operation_->error) diagnostic_operation_->error = error;
    };

    if (payload[0] == 0x43 || payload[0] == 0x47) {
        auto decoded = payload[0] == 0x43 ? protocol::decodeStoredDtcs(message) : protocol::decodePendingDtcs(message);
        if (!decoded) { remember_error(decoded.error()); return; }
        diagnostic_operation_->dtcs.insert(diagnostic_operation_->dtcs.end(), decoded->begin(), decoded->end());
        return;
    }
    if (payload[0] == 0x42 && diagnostic_operation_->freeze_frame_dtc) {
        const auto frame = protocol::decodeFreezeFrameZero(message, message.payload().size() > 1 ? message.payload()[1] : 0, *diagnostic_operation_->freeze_frame_dtc);
        if (!frame) { remember_error(frame.error()); return; }
        const auto match = std::find_if(diagnostic_operation_->dtcs.begin(), diagnostic_operation_->dtcs.end(), [&](const DtcRecord& record) {
            return record.code == frame->dtc_code && record.ecu_address == message.ecu_address;
        });
        if (match != diagnostic_operation_->dtcs.end()) match->freeze_frame = *frame;
        return;
    }
    if (payload[0] == 0x49 && payload.size() >= 2) {
        const auto decoded = protocol::decodeMode09Metadata(message, payload[1]);
        if (!decoded) { remember_error(decoded.error()); return; }
        const auto existing = std::find_if(diagnostic_operation_->metadata.begin(), diagnostic_operation_->metadata.end(), [&](const EcuMetadata& item) {
            return item.ecu_address == decoded->ecu_address;
        });
        if (existing == diagnostic_operation_->metadata.end()) {
            diagnostic_operation_->metadata.push_back(*decoded);
        } else if (const auto merged = protocol::mergeMode09Metadata(*existing, *decoded); !merged) {
            remember_error(merged.error());
        }
        return;
    }
    if (payload[0] == 0x44 && diagnostic_operation_->kind == DiagnosticOperation::Kind::Clear) {
        const auto parsed = protocol::parseClearDiagnosticResponse(message);
        if (!parsed) remember_error(parsed.error());
        else diagnostic_operation_->positive_clear_response = true;
    }
}

void EngineService::finishScan() {
    if (!diagnostic_operation_) return;
    auto unique = protocol::deduplicateDtcs(diagnostic_operation_->dtcs);
    if (dtc_database_) {
        for (auto& record : unique) {
            const auto definition = dtc_database_->lookupExact(record.code);
            if (!definition) {
                if (!diagnostic_operation_->error) diagnostic_operation_->error = definition.error();
                continue;
            }
            record.description = definition->description;
            record.severity = definition->severity;
            record.likely_failure_points = definition->likely_failure_points;
        }
    }

    DiagnosticSnapshot snapshot{
        .dtcs = unique,
        .ecu_metadata = {},
        .findings = diagnostic_evaluator_.findings(),
        .captured_at = clock_->monotonicNow(),
        .engine_epoch = epoch()};
    {
        std::lock_guard lock(diagnostic_mutex_);
        snapshot.ecu_metadata = ecu_metadata_;
        diagnostic_snapshot_ = snapshot;
    }
    publishEvent({.type = EngineEventType::DiagnosticDataUpdated, .connection_state = connectionState(), .epoch = epoch(), .error = diagnostic_operation_->error});

    if (diagnostic_operation_->kind == DiagnosticOperation::Kind::PostClearScan) {
        if (diagnostic_operation_->audit) {
            diagnostic_operation_->audit->post_clear_dtcs = unique;
            diagnostic_operation_->audit->post_clear_rescan_completed = true;
        }
        const auto error = diagnostic_operation_->error;
        finishClear(error);
        return;
    }

    auto completion = std::move(diagnostic_operation_->scan_completion);
    const auto error = diagnostic_operation_->error;
    diagnostic_operation_.reset();
    if (completion) {
        if (error) completion(tl::make_unexpected(*error));
        else completion(std::move(snapshot));
    }
}

void EngineService::finishIdentification() {
    if (!diagnostic_operation_) return;
    auto metadata = diagnostic_operation_->metadata;
    const auto error = diagnostic_operation_->error;
    auto completion = std::move(diagnostic_operation_->identification_completion);
    {
        std::lock_guard lock(diagnostic_mutex_);
        ecu_metadata_ = metadata;
        diagnostic_snapshot_.ecu_metadata = metadata;
    }
    diagnostic_operation_.reset();
    publishEvent({.type = EngineEventType::DiagnosticDataUpdated, .connection_state = connectionState(), .epoch = epoch(), .error = error});
    if (completion) {
        if (error && metadata.empty()) completion(tl::make_unexpected(*error));
        else completion(std::move(metadata));
    }
}

void EngineService::finishClear(std::optional<Error> error) {
    if (!diagnostic_operation_ || !diagnostic_operation_->audit) return;
    auto audit = *diagnostic_operation_->audit;
    audit.completed_at = clock_->monotonicNow();
    audit.completed_utc = clock_->utcNow();
    audit.error = error;
    auto completion = std::move(diagnostic_operation_->clear_completion);
    {
        std::lock_guard lock(diagnostic_mutex_);
        mode04_audits_.push_back(audit);
    }
    diagnostic_operation_.reset();
    clear_settle_at_.reset();
    publishEvent({.type = EngineEventType::Mode04AuditUpdated, .connection_state = connectionState(), .epoch = epoch(), .error = error});
    if (completion) {
        if (error) completion(tl::make_unexpected(*error));
        else completion(std::move(audit));
    }
}

void EngineService::cancelDiagnosticOperation(const Error& error) {
    if (!diagnostic_operation_) return;
    if ((diagnostic_operation_->kind == DiagnosticOperation::Kind::Clear ||
         diagnostic_operation_->kind == DiagnosticOperation::Kind::PostClearScan) &&
        diagnostic_operation_->audit) {
        finishClear(error);
        return;
    }
    auto scan_completion = std::move(diagnostic_operation_->scan_completion);
    auto identification_completion = std::move(diagnostic_operation_->identification_completion);
    diagnostic_operation_.reset();
    if (scan_completion) scan_completion(tl::make_unexpected(error));
    if (identification_completion) identification_completion(tl::make_unexpected(error));
}

void EngineService::invalidateClearPreparation() { clear_token_.reset(); }

Result<void> EngineService::validateClearSafety() const {
    if (!source_ || !active_config_) return tl::make_unexpected(invalidState("Clear preparation requires an active source"));
    if (source_->type() != DataSourceType::SerialElm327 && source_->type() != DataSourceType::SocketCan) {
        return makeError(ErrorCode::DiagnosticsSafetyRejected, "Clearing diagnostic information is available only for a physical vehicle source");
    }
    if (source_->connectionState() != ConnectionState::Ready || connectionState() != ConnectionState::Ready) {
        return makeError(ErrorCode::DiagnosticsSafetyRejected, "The physical source must be ready before clearing diagnostic information");
    }
    if (diagnostic_operation_) return makeError(ErrorCode::DiagnosticsOperationInProgress, "Another diagnostic operation is active");

    const auto sample = telemetry_store_.snapshot().get(MetricId::VehicleSpeed);
    if (sample.quality != SampleQuality::Valid) {
        return makeError(ErrorCode::DiagnosticsSafetyRejected, "A valid vehicle-speed sample is required before clearing diagnostic information");
    }
    const auto* descriptor = protocol::findMode01PidDescriptor(0x0D);
    const auto now = clock_->monotonicNow();
    if (!descriptor || sample.monotonic_ts > now || now - sample.monotonic_ts > descriptor->stale_after) {
        return makeError(ErrorCode::DiagnosticsSafetyRejected, "The vehicle-speed sample is stale");
    }
    if (sample.value > 0.5) {
        return makeError(ErrorCode::DiagnosticsSafetyRejected, "The vehicle must be stationary before clearing diagnostic information");
    }
    return {};
}

std::optional<std::string> EngineService::currentVehicleIdentity() const {
    std::lock_guard lock(diagnostic_mutex_);
    std::vector<std::string> vins;
    for (const auto& metadata : ecu_metadata_) {
        if (!metadata.vin.empty()) vins.push_back(metadata.vin);
    }
    if (vins.empty()) return std::nullopt;
    std::sort(vins.begin(), vins.end());
    vins.erase(std::unique(vins.begin(), vins.end()), vins.end());
    std::string identity;
    for (const auto& vin : vins) {
        if (!identity.empty()) identity.push_back('|');
        identity += vin;
    }
    return identity;
}

void EngineService::completeUnsupported(EngineCompletion completion, const char* operation) {
    if (completion) completion(makeError(ErrorCode::DiagnosticsUnsupported, std::string{operation} + " is not available until its dedicated service stage"));
}

} // namespace revdash::core
