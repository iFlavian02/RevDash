#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "revdash/core/diagnostic_types.hpp"

namespace revdash::diagnostics {

struct DiagnosticRuleConfig {
    std::chrono::steady_clock::duration vacuum_idle_window{std::chrono::seconds{20}};
    std::chrono::steady_clock::duration vacuum_load_window{std::chrono::seconds{10}};
    std::chrono::steady_clock::duration catalyst_window{std::chrono::seconds{30}};
    std::chrono::steady_clock::duration thermostat_observation_window{std::chrono::minutes{10}};
    std::chrono::steady_clock::duration charging_window{std::chrono::seconds{15}};
    std::chrono::steady_clock::duration stable_clear_window{std::chrono::seconds{30}};

    double warm_coolant_c{70.0};
    double idle_min_rpm{600.0};
    double idle_max_rpm{1'100.0};
    double idle_max_speed_kph{2.0};
    double idle_max_load_percent{30.0};
    double vacuum_min_ltft_percent{12.0};
    double vacuum_loaded_min_rpm{1'500.0};
    double vacuum_loaded_min_load_percent{35.0};
    double vacuum_converged_ltft_percent{8.0};
    double vacuum_min_trim_drop_percent{5.0};

    double catalyst_min_coolant_c{75.0};
    double catalyst_min_upstream_swing_v{0.50};
    double catalyst_failure_swing_ratio{0.75};
    double catalyst_clear_swing_ratio{0.45};
    double catalyst_max_rpm_range{300.0};
    double catalyst_max_load_range_percent{12.0};

    double thermostat_max_cold_start_delta_c{10.0};
    double thermostat_max_cold_start_c{50.0};
    double thermostat_min_expected_c{75.0};
    double thermostat_clear_c{82.0};

    double charging_min_rpm{1'000.0};
    double charging_low_voltage_v{11.5};
    double charging_high_voltage_v{16.0};
};

// A topology must be supplied by supported-PID/topology discovery before the
// catalyst rule can run. Metrics are not guessed from their numeric order.
struct OxygenSensorTopology {
    core::MetricId upstream{core::MetricId::O2Sensor1Voltage};
    core::MetricId downstream{core::MetricId::O2Sensor2Voltage};
};

class DiagnosticRuleEvaluator final {
public:
    explicit DiagnosticRuleEvaluator(DiagnosticRuleConfig config = {});
    ~DiagnosticRuleEvaluator();

    DiagnosticRuleEvaluator(const DiagnosticRuleEvaluator&) = delete;
    DiagnosticRuleEvaluator& operator=(const DiagnosticRuleEvaluator&) = delete;
    DiagnosticRuleEvaluator(DiagnosticRuleEvaluator&&) = delete;
    DiagnosticRuleEvaluator& operator=(DiagnosticRuleEvaluator&&) = delete;

    void ingest(const core::TelemetrySample& sample);
    [[nodiscard]] bool evaluate(core::MonotonicTimePoint now);
    [[nodiscard]] std::vector<core::DiagnosticFinding> findings() const;

    void setOxygenSensorTopology(std::optional<OxygenSensorTopology> topology);
    void setEpoch(std::uint64_t epoch);
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] const DiagnosticRuleConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace revdash::diagnostics
