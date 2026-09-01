#include "revdash/diagnostics/rule_evaluator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "revdash/core/metric_aggregator.hpp"

namespace revdash::diagnostics {
namespace {

using core::DiagnosticFinding;
using core::MetricId;
using core::MonotonicClock;
using core::MonotonicTimePoint;
using core::SampleQuality;
using core::Severity;
using core::TelemetrySample;

enum class RuleOutcome : std::uint8_t { Inapplicable, Triggered, Clear };

struct WindowValues {
    std::vector<double> values;

    [[nodiscard]] double minimum() const { return *std::min_element(values.begin(), values.end()); }
    [[nodiscard]] double maximum() const { return *std::max_element(values.begin(), values.end()); }
    [[nodiscard]] double mean() const {
        double sum = 0.0;
        for (const auto value : values) sum += value;
        return sum / static_cast<double>(values.size());
    }
    [[nodiscard]] bool allBetween(double low, double high) const {
        return std::ranges::all_of(values, [low, high](double value) { return value >= low && value <= high; });
    }
};

struct RuleLifecycle {
    std::optional<MonotonicTimePoint> clear_since;
};

struct RuleDefinition {
    const char* id;
    const char* version;
    Severity severity;
    const char* title;
    const char* description;
};

constexpr RuleDefinition kVacuumLeak{
    "HEURISTIC_VACUUM_LEAK", "1.0", Severity::Warning,
    "Possible intake vacuum leak",
    "Sustained positive long-term fuel trim at idle converged under increased engine load. This is an advisory heuristic, not a definitive diagnosis."};
constexpr RuleDefinition kCatalystEfficiency{
    "HEURISTIC_CATALYST_EFFICIENCY", "1.0", Severity::Warning,
    "Possible reduced catalyst oxygen-storage efficiency",
    "A supported downstream oxygen-sensor signal tracked the upstream signal during a warm, steady operating window. Confirm with approved service procedures."};
constexpr RuleDefinition kThermostat{
    "HEURISTIC_THERMOSTAT_STUCK_OPEN", "1.0", Severity::Advisory,
    "Possible thermostat stuck open",
    "Coolant temperature did not reach the conservative warmup threshold after a validated cold-start observation window."};
constexpr RuleDefinition kCharging{
    "HEURISTIC_CHARGING_VOLTAGE", "1.0", Severity::Advisory,
    "Charging-system voltage anomaly",
    "Control-module voltage remained outside broad conservative limits while the warm engine was running. Smart-charging behavior within those limits is not classified as a fault."};

[[nodiscard]] std::string measurement(const char* label, double value, const char* unit) {
    std::ostringstream stream;
    stream << label << ": " << std::fixed << std::setprecision(1) << value << unit;
    return stream.str();
}

[[nodiscard]] bool isOxygenVoltageMetric(MetricId metric) noexcept {
    return metric >= MetricId::O2Sensor1Voltage && metric <= MetricId::O2Sensor8Voltage;
}

} // namespace

struct DiagnosticRuleEvaluator::Impl {
    explicit Impl(DiagnosticRuleConfig initial_config) : config(std::move(initial_config)) {}

    [[nodiscard]] MonotonicClock::duration retention() const noexcept {
        return std::max({config.vacuum_idle_window + config.vacuum_load_window,
                         config.catalyst_window,
                         config.thermostat_observation_window,
                         config.charging_window,
                         config.stable_clear_window}) + std::chrono::seconds{10};
    }

    void resetTransient() {
        for (auto& samples : history) samples.clear();
        for (auto& sample : last) sample.reset();
        vacuum_idle_mean.reset();
        vacuum_idle_seen.reset();
        thermostat_start.reset();
        thermostat_max_c = 0.0;
        for (auto& lifecycle : lifecycles) lifecycle.clear_since.reset();
    }

    void invalidateIncompleteWindows(MetricId metric) {
        history[static_cast<std::size_t>(metric)].clear();
        if (metric == MetricId::Rpm || metric == MetricId::VehicleSpeed || metric == MetricId::EngineLoad ||
            metric == MetricId::CoolantTemp || metric == MetricId::LongTermFuelTrim1) {
            vacuum_idle_mean.reset();
            vacuum_idle_seen.reset();
        }
        if (metric == MetricId::Rpm || metric == MetricId::CoolantTemp || metric == MetricId::AmbientAirTemp) {
            thermostat_start.reset();
            thermostat_max_c = 0.0;
        }
    }

    [[nodiscard]] std::optional<WindowValues> window(MetricId metric, MonotonicTimePoint now, MonotonicClock::duration duration) {
        const auto index = static_cast<std::size_t>(metric);
        if (index >= core::kMetricCount || !last[index] || last[index]->quality != SampleQuality::Valid ||
            now < last[index]->monotonic_ts || now - last[index]->monotonic_ts > core::MetricAggregator::staleAfter(metric)) {
            if (index < core::kMetricCount) history[index].clear();
            return std::nullopt;
        }

        auto& samples = history[index];
        const auto start = now - duration;
        while (!samples.empty() && samples.front().monotonic_ts < start - core::MetricAggregator::staleAfter(metric)) samples.pop_front();
        if (samples.empty()) return std::nullopt;

        WindowValues result;
        MonotonicTimePoint previous{};
        bool have_previous = false;
        for (const auto& sample : samples) {
            if (sample.monotonic_ts < start || sample.monotonic_ts > now) continue;
            if (have_previous && sample.monotonic_ts - previous > core::MetricAggregator::staleAfter(metric) * 2) return std::nullopt;
            result.values.push_back(sample.value);
            previous = sample.monotonic_ts;
            have_previous = true;
        }
        if (result.values.empty()) return std::nullopt;
        if (duration > MonotonicClock::duration::zero()) {
            const auto first_in_window = std::ranges::find_if(samples, [start](const auto& sample) { return sample.monotonic_ts >= start; });
            if (first_in_window == samples.end() || first_in_window->monotonic_ts - start > core::MetricAggregator::staleAfter(metric) || result.values.size() < 2U) return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] bool apply(const RuleDefinition& definition, RuleOutcome outcome, MonotonicTimePoint now, std::vector<std::string> evidence) {
        const auto index = ruleIndex(definition.id);
        auto existing = std::ranges::find_if(findings, [&definition](const auto& finding) { return finding.rule_id == definition.id; });
        if (outcome == RuleOutcome::Inapplicable) {
            lifecycles[index].clear_since.reset();
            if (existing != findings.end()) existing->last_evaluated = now;
            return false;
        }
        if (outcome == RuleOutcome::Triggered) {
            lifecycles[index].clear_since.reset();
            if (existing == findings.end()) {
                findings.push_back(DiagnosticFinding{
                    .rule_id = definition.id,
                    .rule_version = definition.version,
                    .severity = definition.severity,
                    .title = definition.title,
                    .description = definition.description,
                    .evidence = std::move(evidence),
                    .first_detected = now,
                    .last_seen = now,
                    .last_evaluated = now,
                    .resolved_at = std::nullopt,
                    .active = true});
                return true;
            }
            existing->last_evaluated = now;
            existing->last_seen = now;
            existing->evidence = std::move(evidence);
            const bool reactivated = !existing->active;
            existing->active = true;
            existing->resolved_at.reset();
            return reactivated;
        }

        if (existing == findings.end() || !existing->active) return false;
        existing->last_evaluated = now;
        if (!lifecycles[index].clear_since) lifecycles[index].clear_since = now;
        if (now - *lifecycles[index].clear_since < config.stable_clear_window) return false;
        existing->active = false;
        existing->resolved_at = now;
        lifecycles[index].clear_since.reset();
        return true;
    }

    [[nodiscard]] static std::size_t ruleIndex(std::string_view id) noexcept {
        if (id == kVacuumLeak.id) return 0;
        if (id == kCatalystEfficiency.id) return 1;
        if (id == kThermostat.id) return 2;
        return 3;
    }

    [[nodiscard]] bool evaluateVacuum(MonotonicTimePoint now) {
        const auto clear_trim = window(MetricId::LongTermFuelTrim1, now, config.stable_clear_window);
        const auto warm = window(MetricId::CoolantTemp, now, MonotonicClock::duration::zero());
        const auto running = window(MetricId::Rpm, now, config.stable_clear_window);
        const auto current_speed = window(MetricId::VehicleSpeed, now, config.stable_clear_window);
        const auto current_load = window(MetricId::EngineLoad, now, config.stable_clear_window);
        const auto active_finding = std::ranges::find_if(findings, [](const auto& finding) { return finding.rule_id == kVacuumLeak.id && finding.active; });
        if (active_finding != findings.end() && clear_trim && warm && running && current_speed && current_load &&
            warm->values.back() >= config.warm_coolant_c && running->allBetween(config.idle_min_rpm, config.idle_max_rpm) &&
            current_speed->allBetween(0.0, config.idle_max_speed_kph) && current_load->allBetween(0.0, config.idle_max_load_percent) &&
            clear_trim->allBetween(-config.vacuum_converged_ltft_percent, config.vacuum_converged_ltft_percent)) {
            return apply(kVacuumLeak, RuleOutcome::Clear, now, {});
        }

        const auto rpm = window(MetricId::Rpm, now, config.vacuum_idle_window);
        const auto speed = window(MetricId::VehicleSpeed, now, config.vacuum_idle_window);
        const auto load = window(MetricId::EngineLoad, now, config.vacuum_idle_window);
        const auto coolant = window(MetricId::CoolantTemp, now, config.vacuum_idle_window);
        const auto trim = window(MetricId::LongTermFuelTrim1, now, config.vacuum_idle_window);
        if (rpm && speed && load && coolant && trim && rpm->allBetween(config.idle_min_rpm, config.idle_max_rpm) &&
            speed->allBetween(0.0, config.idle_max_speed_kph) && load->allBetween(0.0, config.idle_max_load_percent) &&
            coolant->minimum() >= config.warm_coolant_c && trim->minimum() >= config.vacuum_min_ltft_percent) {
            vacuum_idle_mean = trim->mean();
            vacuum_idle_seen = now;
        }

        if (!vacuum_idle_mean || !vacuum_idle_seen || now - *vacuum_idle_seen > config.vacuum_idle_window + config.vacuum_load_window + std::chrono::minutes{2}) {
            return apply(kVacuumLeak, RuleOutcome::Inapplicable, now, {});
        }

        const auto loaded_rpm = window(MetricId::Rpm, now, config.vacuum_load_window);
        const auto loaded_load = window(MetricId::EngineLoad, now, config.vacuum_load_window);
        const auto loaded_coolant = window(MetricId::CoolantTemp, now, config.vacuum_load_window);
        const auto loaded_trim = window(MetricId::LongTermFuelTrim1, now, config.vacuum_load_window);
        const auto convergence_limit = std::min(config.vacuum_converged_ltft_percent, *vacuum_idle_mean - config.vacuum_min_trim_drop_percent);
        if (!loaded_rpm || !loaded_load || !loaded_coolant || !loaded_trim ||
            loaded_rpm->minimum() < config.vacuum_loaded_min_rpm || loaded_load->minimum() < config.vacuum_loaded_min_load_percent ||
            loaded_coolant->minimum() < config.warm_coolant_c || loaded_trim->maximum() > convergence_limit) {
            return apply(kVacuumLeak, RuleOutcome::Inapplicable, now, {});
        }

        return apply(kVacuumLeak, RuleOutcome::Triggered, now,
                     {measurement("Sustained idle LTFT", *vacuum_idle_mean, "%"),
                      measurement("Loaded LTFT", loaded_trim->mean(), "%"),
                      measurement("Loaded engine load", loaded_load->mean(), "%")});
    }

    [[nodiscard]] bool evaluateCatalyst(MonotonicTimePoint now) {
        if (!oxygen_topology || !isOxygenVoltageMetric(oxygen_topology->upstream) || !isOxygenVoltageMetric(oxygen_topology->downstream) ||
            oxygen_topology->upstream == oxygen_topology->downstream) {
            return apply(kCatalystEfficiency, RuleOutcome::Inapplicable, now, {});
        }
        const auto upstream = window(oxygen_topology->upstream, now, config.catalyst_window);
        const auto downstream = window(oxygen_topology->downstream, now, config.catalyst_window);
        const auto rpm = window(MetricId::Rpm, now, config.catalyst_window);
        const auto load = window(MetricId::EngineLoad, now, config.catalyst_window);
        const auto coolant = window(MetricId::CoolantTemp, now, config.catalyst_window);
        if (!upstream || !downstream || !rpm || !load || !coolant || coolant->minimum() < config.catalyst_min_coolant_c ||
            rpm->maximum() - rpm->minimum() > config.catalyst_max_rpm_range ||
            load->maximum() - load->minimum() > config.catalyst_max_load_range_percent) {
            return apply(kCatalystEfficiency, RuleOutcome::Inapplicable, now, {});
        }
        const auto upstream_swing = upstream->maximum() - upstream->minimum();
        if (upstream_swing < config.catalyst_min_upstream_swing_v) return apply(kCatalystEfficiency, RuleOutcome::Inapplicable, now, {});
        const auto downstream_swing = downstream->maximum() - downstream->minimum();
        const auto ratio = downstream_swing / upstream_swing;
        if (ratio >= config.catalyst_failure_swing_ratio) {
            return apply(kCatalystEfficiency, RuleOutcome::Triggered, now,
                         {measurement("Upstream O2 swing", upstream_swing, " V"),
                          measurement("Downstream O2 swing", downstream_swing, " V"),
                          measurement("Swing ratio", ratio, "")});
        }
        if (ratio <= config.catalyst_clear_swing_ratio) return apply(kCatalystEfficiency, RuleOutcome::Clear, now, {});
        return apply(kCatalystEfficiency, RuleOutcome::Inapplicable, now, {});
    }

    [[nodiscard]] bool evaluateThermostat(MonotonicTimePoint now) {
        const auto rpm = window(MetricId::Rpm, now, MonotonicClock::duration::zero());
        const auto coolant = window(MetricId::CoolantTemp, now, MonotonicClock::duration::zero());
        const auto ambient = window(MetricId::AmbientAirTemp, now, MonotonicClock::duration::zero());
        if (!rpm || !coolant || !ambient || rpm->values.back() < 500.0) {
            thermostat_start.reset();
            thermostat_max_c = 0.0;
            return apply(kThermostat, RuleOutcome::Inapplicable, now, {});
        }
        const auto coolant_c = coolant->values.back();
        if (coolant_c >= config.thermostat_clear_c) return apply(kThermostat, RuleOutcome::Clear, now, {});
        if (!thermostat_start) {
            if (coolant_c > config.thermostat_max_cold_start_c || std::abs(coolant_c - ambient->values.back()) > config.thermostat_max_cold_start_delta_c) {
                return apply(kThermostat, RuleOutcome::Inapplicable, now, {});
            }
            thermostat_start = now;
            thermostat_start_c = coolant_c;
            thermostat_ambient_c = ambient->values.back();
            thermostat_max_c = coolant_c;
        }
        thermostat_max_c = std::max(thermostat_max_c, coolant_c);
        if (thermostat_max_c >= config.thermostat_min_expected_c) {
            thermostat_start.reset();
            return apply(kThermostat, RuleOutcome::Inapplicable, now, {});
        }
        if (now - *thermostat_start < config.thermostat_observation_window) return apply(kThermostat, RuleOutcome::Inapplicable, now, {});
        return apply(kThermostat, RuleOutcome::Triggered, now,
                     {measurement("Cold-start coolant", thermostat_start_c, " degC"),
                      measurement("Ambient", thermostat_ambient_c, " degC"),
                      measurement("Maximum observed coolant", thermostat_max_c, " degC")});
    }

    [[nodiscard]] bool evaluateCharging(MonotonicTimePoint now) {
        const auto voltage = window(MetricId::ModuleVoltage, now, config.charging_window);
        const auto rpm = window(MetricId::Rpm, now, config.charging_window);
        const auto coolant = window(MetricId::CoolantTemp, now, config.charging_window);
        if (!voltage || !rpm || !coolant || rpm->minimum() < config.charging_min_rpm || coolant->minimum() < config.warm_coolant_c) {
            return apply(kCharging, RuleOutcome::Inapplicable, now, {});
        }
        const bool low = voltage->maximum() < config.charging_low_voltage_v;
        const bool high = voltage->minimum() > config.charging_high_voltage_v;
        if (low || high) {
            return apply(kCharging, RuleOutcome::Triggered, now,
                         {measurement("Mean module voltage", voltage->mean(), " V"),
                          std::string{"Conservative allowed band: "} + measurement("", config.charging_low_voltage_v, " V").substr(2) +
                              " to " + measurement("", config.charging_high_voltage_v, " V").substr(2)});
        }
        if (voltage->minimum() >= config.charging_low_voltage_v && voltage->maximum() <= config.charging_high_voltage_v) {
            return apply(kCharging, RuleOutcome::Clear, now, {});
        }
        return apply(kCharging, RuleOutcome::Inapplicable, now, {});
    }

    DiagnosticRuleConfig config;
    mutable std::mutex mutex;
    std::array<std::deque<TelemetrySample>, core::kMetricCount> history{};
    std::array<std::optional<TelemetrySample>, core::kMetricCount> last{};
    std::vector<DiagnosticFinding> findings;
    std::array<RuleLifecycle, 4> lifecycles{};
    std::optional<OxygenSensorTopology> oxygen_topology;
    std::optional<double> vacuum_idle_mean;
    std::optional<MonotonicTimePoint> vacuum_idle_seen;
    std::optional<MonotonicTimePoint> thermostat_start;
    double thermostat_start_c{0.0};
    double thermostat_ambient_c{0.0};
    double thermostat_max_c{0.0};
    std::atomic<std::uint64_t> current_epoch{0};
};

DiagnosticRuleEvaluator::DiagnosticRuleEvaluator(DiagnosticRuleConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
DiagnosticRuleEvaluator::~DiagnosticRuleEvaluator() = default;

void DiagnosticRuleEvaluator::ingest(const TelemetrySample& sample) {
    const auto index = static_cast<std::size_t>(sample.metric_id);
    if (index >= core::kMetricCount) return;
    std::lock_guard lock(impl_->mutex);
    impl_->last[index] = sample;
    if (sample.quality != SampleQuality::Valid) {
        impl_->invalidateIncompleteWindows(sample.metric_id);
        return;
    }
    auto& samples = impl_->history[index];
    const auto position = std::upper_bound(samples.begin(), samples.end(), sample.monotonic_ts,
                                           [](const auto& timestamp, const auto& entry) { return timestamp < entry.monotonic_ts; });
    samples.insert(position, sample);
    while (!samples.empty() && sample.monotonic_ts - samples.front().monotonic_ts > impl_->retention()) samples.pop_front();
}

bool DiagnosticRuleEvaluator::evaluate(MonotonicTimePoint now) {
    std::lock_guard lock(impl_->mutex);
    bool changed = impl_->evaluateVacuum(now);
    changed = impl_->evaluateCatalyst(now) || changed;
    changed = impl_->evaluateThermostat(now) || changed;
    changed = impl_->evaluateCharging(now) || changed;
    return changed;
}

std::vector<DiagnosticFinding> DiagnosticRuleEvaluator::findings() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->findings;
}

void DiagnosticRuleEvaluator::setOxygenSensorTopology(std::optional<OxygenSensorTopology> topology) {
    std::lock_guard lock(impl_->mutex);
    impl_->oxygen_topology = std::move(topology);
    impl_->lifecycles[1].clear_since.reset();
}

void DiagnosticRuleEvaluator::setEpoch(std::uint64_t epoch) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->current_epoch.exchange(epoch, std::memory_order_acq_rel) == epoch) return;
    impl_->resetTransient();
    impl_->findings.clear();
}

std::uint64_t DiagnosticRuleEvaluator::epoch() const noexcept { return impl_->current_epoch.load(std::memory_order_acquire); }
const DiagnosticRuleConfig& DiagnosticRuleEvaluator::config() const noexcept { return impl_->config; }

} // namespace revdash::diagnostics
