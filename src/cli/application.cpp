#include "revdash/cli/application.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "revdash/core/engine_service.hpp"
#include "revdash/drivers/elm327.hpp"
#include "revdash/drivers/playback.hpp"
#include "revdash/drivers/serial_transport.hpp"
#include "revdash/drivers/synthetic.hpp"
#include "revdash/session/csv_exporter.hpp"
#include "revdash/session/session_recorder.hpp"

namespace revdash::cli {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::atomic_bool console_interrupted{false};

void handleInterrupt(int) { console_interrupted.store(true, std::memory_order_relaxed); }

struct SourceOptions {
    std::string source{"synthetic"};
    std::string port{"COM1"};
    std::uint32_t baud{38400};
    std::string session;
    double speed{1.0};
    std::uint32_t seed{12345};
    bool misfire{false};
    bool vacuum_leak{false};
    bool thermostat{false};
    bool second_ecu{false};
};

struct TimedOptions {
    double duration_seconds{0.0};
    std::uint32_t interval_ms{500};
};

struct SourceBinding {
    std::unique_ptr<core::IDataSource> source;
    core::DataSourceConfig config;
};

template <typename T, typename Start>
core::Result<T> await(Start&& start, std::chrono::seconds timeout = 15s) {
    auto promise = std::make_shared<std::promise<core::Result<T>>>();
    auto future = promise->get_future();
    start([promise](core::Result<T> result) mutable { promise->set_value(std::move(result)); });
    if (future.wait_for(timeout) != std::future_status::ready) {
        return core::makeError(core::ErrorCode::TransportTimeout, "The backend operation timed out");
    }
    return future.get();
}

template <typename Start>
core::Result<void> awaitVoid(Start&& start, std::chrono::seconds timeout = 15s) {
    return await<void>(std::forward<Start>(start), timeout);
}

int exitCodeFor(const core::Error& error) {
    if (error.domain == core::ErrorDomain::Transport) return static_cast<int>(ExitCode::Connection);
    if (error.domain == core::ErrorDomain::Protocol) return static_cast<int>(ExitCode::Protocol);
    if (error.domain == core::ErrorDomain::Storage || error.domain == core::ErrorDomain::Session) {
        return static_cast<int>(ExitCode::Io);
    }
    if (error.code == core::toString(core::ErrorCode::DiagnosticsSafetyRejected) ||
        error.code == core::toString(core::ErrorCode::DiagnosticsTokenInvalid) ||
        error.code == core::toString(core::ErrorCode::DiagnosticsTokenExpired)) {
        return static_cast<int>(ExitCode::SafetyRejection);
    }
    return static_cast<int>(ExitCode::Protocol);
}

void emitError(std::ostream& stream, bool jsonl, const core::Error& error) {
    if (jsonl) {
        stream << Json{{"type", "error"}, {"domain", core::toString(error.domain)}, {"code", error.code},
                       {"message", error.message}, {"retryable", error.retryable}, {"context", error.context}}
                      .dump()
               << '\n';
        return;
    }
    stream << "error: " << error.message << " [" << error.code << ']';
    if (!error.context.empty()) stream << " (" << error.context << ')';
    stream << '\n';
}

void table(std::ostream& output, const std::vector<std::string>& headings,
           const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::size_t> widths;
    widths.reserve(headings.size());
    for (const auto& heading : headings) widths.push_back(heading.size());
    for (const auto& row : rows) {
        for (std::size_t index = 0; index < std::min(row.size(), widths.size()); ++index) {
            widths[index] = std::max(widths[index], row[index].size());
        }
    }
    const auto printRow = [&](const auto& row) {
        for (std::size_t index = 0; index < widths.size(); ++index) {
            if (index != 0) output << "  ";
            output << std::left << std::setw(static_cast<int>(widths[index]))
                   << (index < row.size() ? row[index] : std::string{});
        }
        output << '\n';
    };
    printRow(headings);
    std::vector<std::string> separator;
    for (const auto width : widths) separator.emplace_back(width, '-');
    printRow(separator);
    for (const auto& row : rows) printRow(row);
}

void addSourceOptions(CLI::App& command, SourceOptions& options, bool allow_playback = true) {
    auto* source = command.add_option("--source", options.source, "Source: synthetic, serial, or playback")
                       ->capture_default_str();
    if (allow_playback) source->check(CLI::IsMember({"synthetic", "serial", "playback"}));
    else source->check(CLI::IsMember({"synthetic", "serial"}));
    command.add_option("--port", options.port, "Serial port name")->capture_default_str();
    command.add_option("--baud", options.baud, "Serial baud rate")->capture_default_str();
    command.add_option("--session", options.session, "Playback session path");
    command.add_option("--speed", options.speed, "Playback speed multiplier")->check(CLI::PositiveNumber);
    command.add_option("--seed", options.seed, "Synthetic deterministic seed");
    command.add_flag("--misfire", options.misfire, "Inject a synthetic misfire");
    command.add_flag("--vacuum-leak", options.vacuum_leak, "Inject a synthetic vacuum leak");
    command.add_flag("--thermostat", options.thermostat, "Inject a synthetic thermostat fault");
    command.add_flag("--second-ecu", options.second_ecu, "Include a second synthetic ECU");
}

void addTimedOptions(CLI::App& command, TimedOptions& options, double default_duration) {
    options.duration_seconds = default_duration;
    command.add_option("--duration", options.duration_seconds, "Seconds to run; zero means until Ctrl+C")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command.add_option("--interval-ms", options.interval_ms, "Output interval in milliseconds")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
}

void addSyntheticOptions(CLI::App& command, SourceOptions& options) {
    command.add_option("--seed", options.seed, "Synthetic deterministic seed");
    command.add_flag("--misfire", options.misfire, "Inject a synthetic misfire");
    command.add_flag("--vacuum-leak", options.vacuum_leak, "Inject a synthetic vacuum leak");
    command.add_flag("--thermostat", options.thermostat, "Inject a synthetic thermostat fault");
    command.add_flag("--second-ecu", options.second_ecu, "Include a second synthetic ECU");
}

core::Result<SourceBinding> makeSource(const SourceOptions& options) {
    if (options.source == "serial") {
        auto normalized = drivers::normalizeSerialPortName(options.port);
        if (!normalized) return tl::make_unexpected(normalized.error());
        if (!drivers::isSupportedSerialBaudRate(options.baud)) {
            return core::makeError(core::ErrorCode::TransportNotConnected, "Unsupported serial baud rate",
                                   false, std::to_string(options.baud));
        }
        return SourceBinding{std::make_unique<drivers::Elm327DataSource>(),
                             core::SerialConfig{.port_name = *normalized, .baud_rate = options.baud}};
    }
    if (options.source == "playback") {
        if (options.session.empty()) {
            return core::makeError(core::ErrorCode::SessionInvalidFormat,
                                   "Playback requires --session");
        }
        return SourceBinding{std::make_unique<drivers::PlaybackDataSource>(),
                             core::PlaybackConfig{.session_file_path = options.session,
                                                  .speed_multiplier = options.speed}};
    }
    return SourceBinding{
        std::make_unique<drivers::SyntheticDataSource>(),
        core::SyntheticConfig{.deterministic_seed = options.seed,
                              .inject_misfire = options.misfire,
                              .inject_vacuum_leak = options.vacuum_leak,
                              .inject_thermostat_fault = options.thermostat,
                              .include_second_ecu = options.second_ecu}};
}

core::Result<void> connect(core::EngineService& engine, const SourceOptions& options) {
    auto binding = makeSource(options);
    if (!binding) return tl::make_unexpected(binding.error());
    auto installed = awaitVoid([&](auto completion) {
        engine.setSource(std::move(binding->source), std::move(completion));
    });
    if (!installed) return installed;
    auto connected = awaitVoid([&](auto completion) { engine.connect(binding->config, std::move(completion)); });
    if (!connected) return connected;
    engine.setSupportedPids({0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                             0x2F, 0x42, 0x06, 0x07, 0x08, 0x09});
    return core::makeSuccess();
}

void disconnect(core::EngineService& engine) {
    static_cast<void>(awaitVoid([&](auto completion) { engine.disconnect(std::move(completion)); }, 5s));
}

Json sampleJson(const core::TelemetrySample& sample) {
    Json result{{"type", "telemetry"}, {"metric", core::toString(sample.metric_id)},
                {"unit", core::getCanonicalUnit(sample.metric_id)}, {"quality", core::toString(sample.quality)},
                {"sequence", sample.sequence_number}};
    if (sample.isValid()) result["value"] = sample.value;
    else result["value"] = nullptr;
    return result;
}

void emitTelemetry(core::EngineService& engine, bool jsonl, std::ostream& output) {
    const auto snapshot = engine.telemetrySnapshot();
    std::vector<std::vector<std::string>> rows;
    for (const auto& sample : snapshot.samples) {
        if (sample.quality == core::SampleQuality::Unsupported) continue;
        if (jsonl) {
            output << sampleJson(sample).dump() << '\n';
        } else {
            std::ostringstream value;
            if (sample.isValid()) value << std::fixed << std::setprecision(2) << sample.value;
            else value << '-';
            rows.push_back({std::string{core::toString(sample.metric_id)}, value.str(),
                            std::string{core::getCanonicalUnit(sample.metric_id)},
                            std::string{core::toString(sample.quality)}});
        }
    }
    if (!jsonl) table(output, {"Metric", "Value", "Unit", "Quality"}, rows);
}

int runLive(const SourceOptions& source, const TimedOptions& timed, bool jsonl,
            std::ostream& output, std::ostream& error, InterruptFlag& interrupt,
            bool start_playback = false, std::optional<double> simulation_throttle = std::nullopt) {
    core::EngineService engine;
    if (const auto result = connect(engine, source); !result) {
        emitError(error, jsonl, result.error());
        return exitCodeFor(result.error());
    }
    if (start_playback) {
        const auto started = awaitVoid([&](auto completion) { engine.startPlayback(std::move(completion)); });
        if (!started) {
            emitError(error, jsonl, started.error());
            disconnect(engine);
            return exitCodeFor(started.error());
        }
    }
    if (simulation_throttle) {
        const auto controlled = awaitVoid([&](auto completion) {
            engine.setSimulationThrottle(*simulation_throttle, std::move(completion));
        });
        if (!controlled) {
            emitError(error, jsonl, controlled.error());
            disconnect(engine);
            return exitCodeFor(controlled.error());
        }
    }
    const auto started_at = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>{timed.duration_seconds};
    do {
        emitTelemetry(engine, jsonl, output);
        const auto wake_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{timed.interval_ms};
        while (!interrupt.requested() && std::chrono::steady_clock::now() < wake_at) {
            std::this_thread::sleep_for(10ms);
        }
    } while (!interrupt.requested() &&
             (timed.duration_seconds == 0.0 || std::chrono::steady_clock::now() - started_at < duration));
    if (start_playback) {
        static_cast<void>(awaitVoid([&](auto completion) { engine.stopPlayback(std::move(completion)); }, 5s));
    }
    disconnect(engine);
    return static_cast<int>(ExitCode::Success);
}

Json dtcJson(const core::DtcRecord& dtc) {
    Json ecu = nullptr;
    if (dtc.ecu_address) ecu = dtc.ecu_address->value;
    return Json{{"type", "dtc"}, {"code", dtc.code}, {"status", core::toString(dtc.status)},
                {"severity", core::toString(dtc.severity)}, {"description", dtc.description}, {"ecu", ecu}};
}

int runScan(const SourceOptions& source, bool jsonl, std::ostream& output, std::ostream& error) {
    core::EngineService engine;
    if (const auto result = connect(engine, source); !result) {
        emitError(error, jsonl, result.error());
        return exitCodeFor(result.error());
    }
    const auto scanned = await<core::DiagnosticSnapshot>(
        [&](auto completion) { engine.scan(std::move(completion)); });
    if (!scanned) {
        emitError(error, jsonl, scanned.error());
        disconnect(engine);
        return exitCodeFor(scanned.error());
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto& dtc : scanned->dtcs) {
        if (jsonl) output << dtcJson(dtc).dump() << '\n';
        else rows.push_back({dtc.code, std::string{core::toString(dtc.status)},
                             std::string{core::toString(dtc.severity)}, dtc.description});
    }
    if (!jsonl) table(output, {"DTC", "Status", "Severity", "Description"}, rows);
    disconnect(engine);
    return static_cast<int>(ExitCode::Success);
}

int runIdentify(const SourceOptions& source, bool jsonl, std::ostream& output, std::ostream& error) {
    core::EngineService engine;
    if (const auto result = connect(engine, source); !result) {
        emitError(error, jsonl, result.error());
        return exitCodeFor(result.error());
    }
    const auto identified = await<std::vector<core::EcuMetadata>>(
        [&](auto completion) { engine.identify(std::move(completion)); });
    if (!identified) {
        emitError(error, jsonl, identified.error());
        disconnect(engine);
        return exitCodeFor(identified.error());
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto& ecu : *identified) {
        const auto address = ecu.ecu_address ? std::to_string(ecu.ecu_address->value) : std::string{"-"};
        if (jsonl) {
            output << Json{{"type", "ecu"}, {"address", address}, {"vin", ecu.vin},
                           {"calibration_ids", ecu.calibration_ids}, {"cvns", ecu.cvns},
                           {"protocol", ecu.protocol_name}}
                          .dump()
                   << '\n';
        } else {
            rows.push_back({address, ecu.vin, ecu.protocol_name,
                            ecu.calibration_ids.empty() ? "-" : ecu.calibration_ids.front()});
        }
    }
    if (!jsonl) table(output, {"ECU", "VIN", "Protocol", "Calibration"}, rows);
    disconnect(engine);
    return static_cast<int>(ExitCode::Success);
}

int runRecord(const SourceOptions& source, const TimedOptions& timed, const std::string& destination,
              bool jsonl, std::ostream& output, std::ostream& error, InterruptFlag& interrupt) {
    core::EngineService engine;
    if (const auto result = connect(engine, source); !result) {
        emitError(error, jsonl, result.error());
        return exitCodeFor(result.error());
    }
    session::JsonlSessionRecorder recorder;
    const auto source_type = source.source == "serial" ? core::DataSourceType::SerialElm327
                           : source.source == "playback" ? core::DataSourceType::Playback
                                                          : core::DataSourceType::Synthetic;
    session::SessionHeader header{.uuid = session::generateSessionUuid(),
                                  .utc_start = core::SystemClock::now(),
                                  .source_type = source_type};
    if (source.source == "synthetic") {
        header.simulation = session::SimulationMetadata{.seed = source.seed};
    }
    const auto monotonic_start = core::MonotonicClock::now();
    if (const auto started = recorder.start(destination, std::move(header), monotonic_start); !started) {
        emitError(error, jsonl, started.error());
        disconnect(engine);
        return exitCodeFor(started.error());
    }
    std::mutex recorder_mutex;
    std::optional<core::Error> recorder_error;
    engine.setRecorderHandler([&](const core::RecorderPacket& packet) {
        std::lock_guard lock(recorder_mutex);
        if (recorder_error) return;
        if (auto result = recorder.record(packet.message); !result) recorder_error = result.error();
    });
    const auto started_at = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>{timed.duration_seconds};
    while (!interrupt.requested() &&
           (timed.duration_seconds == 0.0 || std::chrono::steady_clock::now() - started_at < duration)) {
        std::this_thread::sleep_for(10ms);
    }
    disconnect(engine);
    const auto drain_deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < drain_deadline) {
        const auto health = engine.recorderQueueHealth();
        if (health.popped >= health.pushed) break;
        std::this_thread::yield();
    }
    engine.setRecorderHandler({});
    std::lock_guard lock(recorder_mutex);
    if (recorder_error) {
        emitError(error, jsonl, *recorder_error);
        return exitCodeFor(*recorder_error);
    }
    if (const auto loss = recorder.observeQueueDrops(
            engine.recorderQueueHealth().dropped, core::MonotonicClock::now()); !loss) {
        emitError(error, jsonl, loss.error());
        return exitCodeFor(loss.error());
    }
    if (const auto finished = recorder.finish(core::MonotonicClock::now()); !finished) {
        emitError(error, jsonl, finished.error());
        return exitCodeFor(finished.error());
    }
    const auto count = recorder.statistics().total_records;
    if (jsonl) output << Json{{"type", "recording"}, {"path", destination}, {"records", count}}.dump() << '\n';
    else table(output, {"Session", "Records"}, {{destination, std::to_string(count)}});
    return static_cast<int>(ExitCode::Success);
}

int runClear(const SourceOptions& source, bool jsonl, std::istream& input,
             std::ostream& output, std::ostream& error) {
    core::EngineService engine;
    if (const auto result = connect(engine, source); !result) {
        emitError(error, jsonl, result.error());
        return exitCodeFor(result.error());
    }
    const auto speed_deadline = std::chrono::steady_clock::now() + 5s;
    while (!engine.telemetrySnapshot().isValid(core::MetricId::VehicleSpeed) &&
           std::chrono::steady_clock::now() < speed_deadline) std::this_thread::sleep_for(10ms);
    const auto prepared = await<core::ClearDtcPreparation>(
        [&](auto completion) { engine.prepareClear(std::move(completion)); });
    if (!prepared) {
        emitError(error, jsonl, prepared.error());
        disconnect(engine);
        return exitCodeFor(prepared.error());
    }
    if (jsonl) {
        output << Json{{"type", "clear_preparation"}, {"warning", prepared->warning},
                       {"confirmation_token", prepared->confirmation_token}}
                      .dump()
               << '\n';
    } else {
        output << "WARNING: " << prepared->warning << "\nToken: " << prepared->confirmation_token
               << "\nType CLEAR to acknowledge: " << std::flush;
    }
    std::string acknowledgement;
    std::getline(input, acknowledgement);
    if (acknowledgement != "CLEAR") {
        core::Error rejection{.domain = core::ErrorDomain::Diagnostics,
                              .code = std::string{core::toString(core::ErrorCode::DiagnosticsSafetyRejected)},
                              .message = "Clear acknowledgement was rejected",
                              .context = {}};
        emitError(error, jsonl, rejection);
        disconnect(engine);
        return static_cast<int>(ExitCode::SafetyRejection);
    }
    if (!jsonl) output << "Re-enter the token: " << std::flush;
    std::string token;
    std::getline(input, token);
    if (token != prepared->confirmation_token) {
        core::Error mismatch{.domain = core::ErrorDomain::Diagnostics,
                             .code = std::string{core::toString(core::ErrorCode::DiagnosticsTokenInvalid)},
                             .message = "The clear confirmation token does not match",
                             .context = {}};
        emitError(error, jsonl, mismatch);
        disconnect(engine);
        return static_cast<int>(ExitCode::SafetyRejection);
    }
    const auto cleared = await<core::Mode04AuditRecord>(
        [&](auto completion) { engine.confirmClear(token, std::move(completion)); }, 20s);
    if (!cleared) {
        emitError(error, jsonl, cleared.error());
        disconnect(engine);
        return exitCodeFor(cleared.error());
    }
    if (jsonl) output << Json{{"type", "clear_result"}, {"positive_response", cleared->positive_response},
                              {"post_clear_dtcs", cleared->post_clear_dtcs.size()}}.dump() << '\n';
    else table(output, {"Result", "Remaining DTCs"},
               {{cleared->positive_response ? "Cleared" : "No positive response",
                 std::to_string(cleared->post_clear_dtcs.size())}});
    disconnect(engine);
    return static_cast<int>(ExitCode::Success);
}

} // namespace

ConsoleInterruptFlag::ConsoleInterruptFlag() {
    console_interrupted.store(false, std::memory_order_relaxed);
    previous_handler_ = std::signal(SIGINT, handleInterrupt);
}

ConsoleInterruptFlag::~ConsoleInterruptFlag() {
    if (previous_handler_ != SIG_ERR) std::signal(SIGINT, previous_handler_);
}

bool ConsoleInterruptFlag::requested() const noexcept {
    return console_interrupted.load(std::memory_order_relaxed);
}

int run(const std::vector<std::string>& argv, std::istream& input, std::ostream& output,
        std::ostream& error, InterruptFlag& interrupt) {
    CLI::App app{"RevDash OBD-II diagnostic CLI", "revdash_cli"};
    app.require_subcommand(0, 1);
    bool jsonl = false;
    bool version = false;
    app.add_flag("--jsonl", jsonl, "Emit newline-delimited JSON");
    app.add_flag("-v,--version", version, "Display application version");

    auto* sources_command = app.add_subcommand("sources", "List available diagnostic sources");
    SourceOptions live_source;
    TimedOptions live_timed;
    auto* live_command = app.add_subcommand("live", "Stream live telemetry");
    addSourceOptions(*live_command, live_source);
    addTimedOptions(*live_command, live_timed, 0.0);

    SourceOptions scan_source;
    auto* scan_command = app.add_subcommand("scan", "Scan stored and pending DTCs");
    addSourceOptions(*scan_command, scan_source);
    SourceOptions identify_source;
    auto* identify_command = app.add_subcommand("identify", "Read ECU identification data");
    addSourceOptions(*identify_command, identify_source);

    SourceOptions simulate_source;
    simulate_source.source = "synthetic";
    TimedOptions simulate_timed;
    double throttle = 25.0;
    auto* simulate_command = app.add_subcommand("simulate", "Run deterministic synthetic telemetry");
    addSyntheticOptions(*simulate_command, simulate_source);
    addTimedOptions(*simulate_command, simulate_timed, 5.0);
    simulate_command->add_option("--throttle", throttle, "Synthetic throttle percent")
        ->check(CLI::Range(0.0, 100.0));

    SourceOptions record_source;
    TimedOptions record_timed;
    record_timed.interval_ms = 100;
    std::string record_destination;
    auto* record_command = app.add_subcommand("record", "Record a session");
    addSourceOptions(*record_command, record_source, false);
    addTimedOptions(*record_command, record_timed, 5.0);
    record_command->add_option("output", record_destination, "Destination JSONL session")->required();

    SourceOptions playback_source;
    playback_source.source = "playback";
    TimedOptions playback_timed;
    auto* playback_command = app.add_subcommand("playback", "Replay a recorded session");
    playback_command->add_option("session", playback_source.session, "Input JSONL session")->required();
    playback_command->add_option("--speed", playback_source.speed, "Playback speed")->check(CLI::PositiveNumber);
    addTimedOptions(*playback_command, playback_timed, 5.0);

    std::string export_source;
    std::string export_destination;
    std::string preset{"revdash"};
    std::string units{"metric"};
    std::uint32_t export_interval{100};
    auto* export_command = app.add_subcommand("export", "Export a session to CSV");
    export_command->add_option("session", export_source, "Input JSONL session")->required();
    export_command->add_option("output", export_destination, "Destination CSV")->required();
    export_command->add_option("--preset", preset)->check(CLI::IsMember({"revdash", "megalogviewer", "tunerstudio"}));
    export_command->add_option("--units", units)->check(CLI::IsMember({"metric", "imperial"}));
    export_command->add_option("--interval-ms", export_interval)->check(CLI::PositiveNumber);

    SourceOptions clear_source;
    clear_source.source = "serial";
    auto* clear_command = app.add_subcommand("clear", "Clear diagnostic information with guarded confirmation");
    clear_command->add_option("--port", clear_source.port, "Serial port name")->required();
    clear_command->add_option("--baud", clear_source.baud, "Serial baud rate")->capture_default_str();

    try {
        std::vector<std::string> arguments;
        if (argv.size() > 1) arguments.assign(argv.begin() + 1, argv.end());
        std::reverse(arguments.begin(), arguments.end()); // CLI11's vector overload consumes from the back.
        app.parse(arguments);
    } catch (const CLI::ParseError& parse_error) {
        return app.exit(parse_error, output, error) == 0 ? 0 : static_cast<int>(ExitCode::Usage);
    }
    if (version) {
        output << core::kApplicationName << " version " << core::kApplicationVersion << '\n';
        return static_cast<int>(ExitCode::Success);
    }
    if (*sources_command) {
        std::vector<std::vector<std::string>> rows{{"synthetic", "built-in", "ready"},
                                                   {"playback", "session file", "ready"}};
        for (const auto& port : drivers::enumerateSerialPorts()) {
            if (jsonl) output << Json{{"type", "source"}, {"source", "serial"}, {"name", port.port_name},
                                      {"description", port.friendly_name}, {"bluetooth", port.is_bluetooth_spp}}.dump() << '\n';
            else rows.push_back({"serial", port.port_name, port.friendly_name});
        }
        if (jsonl) {
            output << Json{{"type", "source"}, {"source", "synthetic"}, {"name", "built-in"}}.dump() << '\n';
            output << Json{{"type", "source"}, {"source", "playback"}, {"name", "session file"}}.dump() << '\n';
        } else table(output, {"Source", "Name", "Description"}, rows);
        return static_cast<int>(ExitCode::Success);
    }
    if (*live_command) return runLive(live_source, live_timed, jsonl, output, error, interrupt);
    if (*scan_command) return runScan(scan_source, jsonl, output, error);
    if (*identify_command) return runIdentify(identify_source, jsonl, output, error);
    if (*simulate_command) {
        simulate_source.source = "synthetic";
        return runLive(simulate_source, simulate_timed, jsonl, output, error, interrupt, false, throttle);
    }
    if (*record_command) return runRecord(record_source, record_timed, record_destination, jsonl, output, error, interrupt);
    if (*playback_command) return runLive(playback_source, playback_timed, jsonl, output, error, interrupt, true);
    if (*export_command) {
        const auto csv_preset = preset == "megalogviewer" ? session::CsvPreset::MegaLogViewer
                               : preset == "tunerstudio" ? session::CsvPreset::TunerStudio
                                                          : session::CsvPreset::RevDash;
        const auto unit_system = units == "imperial" ? session::UnitSystem::Imperial : session::UnitSystem::Metric;
        const auto exported = session::exportSessionCsv(
            export_source, export_destination,
            {.preset = csv_preset, .units = unit_system, .interval = std::chrono::milliseconds{export_interval}});
        if (!exported) {
            emitError(error, jsonl, exported.error());
            return exitCodeFor(exported.error());
        }
        if (jsonl) output << Json{{"type", "export"}, {"source", export_source},
                                  {"destination", export_destination}}.dump() << '\n';
        else table(output, {"Session", "CSV"}, {{export_source, export_destination}});
        return static_cast<int>(ExitCode::Success);
    }
    if (*clear_command) return runClear(clear_source, jsonl, input, output, error);
    output << app.help();
    return static_cast<int>(ExitCode::Usage);
}

} // namespace revdash::cli
