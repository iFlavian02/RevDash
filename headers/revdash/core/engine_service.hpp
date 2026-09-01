#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "revdash/core/data_source.hpp"
#include "revdash/core/latest_telemetry_store.hpp"
#include "revdash/core/metric_aggregator.hpp"
#include "revdash/core/pipeline_packets.hpp"
#include "revdash/diagnostics/rule_evaluator.hpp"
#include "revdash/drivers/pid_scheduler.hpp"

namespace revdash::core {

enum class EngineEventType : std::uint8_t { ConnectionStateChanged, TelemetryUpdated, DiagnosticFindingsUpdated, Error };

struct EngineEvent {
    EngineEventType type{EngineEventType::ConnectionStateChanged};
    ConnectionState connection_state{ConnectionState::Disconnected};
    std::uint64_t epoch{0};
    std::optional<Error> error;
};

using EngineCompletion = std::function<void(Result<void>)>;
using EngineEventHandler = std::function<void(const EngineEvent&)>;
using RecorderHandler = std::function<void(const RecorderPacket&)>;

// Coordinates one IDataSource and the Qt-independent processing pipeline.
// Public commands are safe from any thread; their completions and event handlers
// execute on the engine worker and presentation layers must marshal as needed.
class EngineService final {
public:
    EngineService();
    ~EngineService();

    EngineService(const EngineService&) = delete;
    EngineService& operator=(const EngineService&) = delete;

    void setSource(std::unique_ptr<IDataSource> source, EngineCompletion completion = {});
    void connect(const DataSourceConfig& config, EngineCompletion completion = {});
    void disconnect(EngineCompletion completion = {});
    void scan(EngineCompletion completion = {});
    void identify(EngineCompletion completion = {});
    void prepareClear(EngineCompletion completion = {});
    void confirmClear(EngineCompletion completion = {});
    void startRecording(EngineCompletion completion = {});
    void stopRecording(EngineCompletion completion = {});
    void startPlayback(EngineCompletion completion = {});
    void stopPlayback(EngineCompletion completion = {});
    void setSimulationThrottle(double percent, EngineCompletion completion = {});
    void setSimulationAmbientTemperature(double celsius, EngineCompletion completion = {});
    void resetSimulation(EngineCompletion completion = {});

    // Exposed for source discovery and deterministic service tests.
    void setSupportedPids(std::vector<std::uint8_t> pids);
    void setOxygenSensorTopology(std::optional<diagnostics::OxygenSensorTopology> topology);

    [[nodiscard]] TelemetrySnapshot telemetrySnapshot() const noexcept;
    [[nodiscard]] std::vector<DiagnosticFinding> diagnosticFindings() const;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] ConnectionState connectionState() const noexcept;
    [[nodiscard]] QueueHealth sourceQueueHealth() const noexcept;
    [[nodiscard]] QueueHealth recorderQueueHealth() const noexcept;
    [[nodiscard]] SubscriptionToken subscribe(EngineEventHandler handler);
    void setRecorderHandler(RecorderHandler handler);

private:
    using Command = std::function<void()>;

    void enqueue(Command command);
    void run(std::stop_token stop_token);
    void recorderRun(std::stop_token stop_token);
    void processCommands();
    void processPackets();
    void dispatchScheduler();
    void publishEvent(EngineEvent event);
    void bindSource();
    void invalidateEpoch();
    void handleSourceState(ConnectionState state, const std::optional<Error>& error);
    void scheduleReconnect();
    void attemptReconnect();
    void completeUnsupported(EngineCompletion completion, const char* operation);

    mutable std::mutex command_mutex_;
    std::condition_variable_any command_ready_;
    std::deque<Command> commands_;
    std::unique_ptr<SourceToEngineQueue> source_to_engine_;
    std::unique_ptr<EngineToRecorderQueue> engine_to_recorder_;

    std::unique_ptr<IDataSource> source_;
    SubscriptionToken source_subscription_;
    drivers::AdaptivePidScheduler scheduler_;
    LatestTelemetryStore telemetry_store_;
    MetricAggregator metric_aggregator_;
    diagnostics::DiagnosticRuleEvaluator diagnostic_evaluator_;
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<ConnectionState> connection_state_{ConnectionState::Disconnected};
    std::optional<DataSourceConfig> active_config_;
    std::optional<MonotonicTimePoint> reconnect_at_;
    std::uint8_t reconnect_attempt_{0};
    bool reconnecting_{false};

    mutable std::mutex event_mutex_;
    struct EventSubscriber;
    std::uint64_t next_subscriber_id_{0};
    std::vector<std::pair<std::uint64_t, std::shared_ptr<EventSubscriber>>> subscribers_;

    mutable std::mutex recorder_mutex_;
    RecorderHandler recorder_handler_;
    std::jthread worker_;
    std::jthread recorder_worker_;
};

} // namespace revdash::core
