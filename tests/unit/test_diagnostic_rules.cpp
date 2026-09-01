#include <chrono>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "revdash/diagnostics/rule_evaluator.hpp"

namespace {

using namespace std::chrono_literals;
using revdash::core::DiagnosticFinding;
using revdash::core::MetricId;
using revdash::core::MonotonicTimePoint;
using revdash::core::SampleQuality;
using revdash::core::TelemetrySample;
using revdash::diagnostics::DiagnosticRuleConfig;
using revdash::diagnostics::DiagnosticRuleEvaluator;

DiagnosticRuleConfig fastConfig() {
    DiagnosticRuleConfig config;
    config.vacuum_idle_window = 2s;
    config.vacuum_load_window = 2s;
    config.catalyst_window = 2s;
    config.thermostat_observation_window = 3s;
    config.charging_window = 2s;
    config.stable_clear_window = 2s;
    return config;
}

TelemetrySample sample(MetricId metric, double value, MonotonicTimePoint time, SampleQuality quality = SampleQuality::Valid) {
    return TelemetrySample{.metric_id = metric, .value = value, .quality = quality, .monotonic_ts = time, .utc_ts = std::nullopt};
}

void ingest(DiagnosticRuleEvaluator& evaluator, MonotonicTimePoint time, double rpm, double speed, double load,
            double coolant, double ambient, double ltft, double voltage) {
    evaluator.ingest(sample(MetricId::Rpm, rpm, time));
    evaluator.ingest(sample(MetricId::VehicleSpeed, speed, time));
    evaluator.ingest(sample(MetricId::EngineLoad, load, time));
    evaluator.ingest(sample(MetricId::CoolantTemp, coolant, time));
    evaluator.ingest(sample(MetricId::AmbientAirTemp, ambient, time));
    evaluator.ingest(sample(MetricId::LongTermFuelTrim1, ltft, time));
    evaluator.ingest(sample(MetricId::ModuleVoltage, voltage, time));
}

std::optional<DiagnosticFinding> finding(const DiagnosticRuleEvaluator& evaluator, std::string_view id) {
    for (const auto& item : evaluator.findings()) {
        if (item.rule_id == id) return item;
    }
    return std::nullopt;
}

void addVacuumLeakSequence(DiagnosticRuleEvaluator& evaluator, MonotonicTimePoint start, double idle_trim = 12.0) {
    for (int second = 0; second <= 2; ++second) {
        const auto now = start + std::chrono::seconds{second};
        ingest(evaluator, now, 800.0, 0.0, 20.0, 85.0, 20.0, idle_trim, 13.5);
        static_cast<void>(evaluator.evaluate(now));
    }
    for (int second = 3; second <= 5; ++second) {
        const auto now = start + std::chrono::seconds{second};
        ingest(evaluator, now, 1'800.0, 30.0, 45.0, 85.0, 20.0, 6.0, 13.5);
        static_cast<void>(evaluator.evaluate(now));
    }
}

} // namespace

TEST_CASE("Vacuum leak rule requires sustained idle trim and convergence under load", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{10s};

    SECTION("positive boundary fixture is deduplicated and captures evidence") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        addVacuumLeakSequence(evaluator, start);
        auto result = finding(evaluator, "HEURISTIC_VACUUM_LEAK");
        REQUIRE(result.has_value());
        REQUIRE(result->active);
        REQUIRE(result->rule_version == "1.0");
        REQUIRE(result->evidence.size() == 3);
        REQUIRE(evaluator.findings().size() == 1);

        ingest(evaluator, start + 6s, 1'800.0, 30.0, 45.0, 85.0, 20.0, 6.0, 13.5);
        static_cast<void>(evaluator.evaluate(start + 6s));
        REQUIRE(evaluator.findings().size() == 1);
        REQUIRE(finding(evaluator, "HEURISTIC_VACUUM_LEAK")->first_detected == result->first_detected);
    }

    SECTION("below-threshold trim is a negative boundary fixture") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        addVacuumLeakSequence(evaluator, start, 11.9);
        REQUIRE_FALSE(finding(evaluator, "HEURISTIC_VACUUM_LEAK").has_value());
    }

    SECTION("missing speed PID prevents applicability") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        for (int second = 0; second <= 5; ++second) {
            const auto now = start + std::chrono::seconds{second};
            evaluator.ingest(sample(MetricId::Rpm, second < 3 ? 800.0 : 1'800.0, now));
            evaluator.ingest(sample(MetricId::EngineLoad, second < 3 ? 20.0 : 45.0, now));
            evaluator.ingest(sample(MetricId::CoolantTemp, 85.0, now));
            evaluator.ingest(sample(MetricId::LongTermFuelTrim1, second < 3 ? 18.0 : 4.0, now));
            static_cast<void>(evaluator.evaluate(now));
        }
        REQUIRE_FALSE(finding(evaluator, "HEURISTIC_VACUUM_LEAK").has_value());
    }
}

TEST_CASE("Invalid and stale data reset incomplete diagnostic windows", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{20s};
    DiagnosticRuleEvaluator evaluator{fastConfig()};
    for (int second = 0; second <= 1; ++second) {
        const auto now = start + std::chrono::seconds{second};
        ingest(evaluator, now, 800.0, 0.0, 20.0, 85.0, 20.0, 18.0, 13.5);
        static_cast<void>(evaluator.evaluate(now));
    }
    evaluator.ingest(sample(MetricId::LongTermFuelTrim1, 0.0, start + 2s, SampleQuality::Invalid));
    static_cast<void>(evaluator.evaluate(start + 2s));
    for (int second = 3; second <= 5; ++second) {
        const auto now = start + std::chrono::seconds{second};
        ingest(evaluator, now, 1'800.0, 30.0, 45.0, 85.0, 20.0, 5.0, 13.5);
        static_cast<void>(evaluator.evaluate(now));
    }
    REQUIRE_FALSE(finding(evaluator, "HEURISTIC_VACUUM_LEAK").has_value());

    DiagnosticRuleEvaluator stale{fastConfig()};
    ingest(stale, start, 800.0, 0.0, 20.0, 85.0, 20.0, 18.0, 13.5);
    static_cast<void>(stale.evaluate(start + 5s));
    REQUIRE_FALSE(finding(stale, "HEURISTIC_VACUUM_LEAK").has_value());

    DiagnosticRuleEvaluator unsupported{fastConfig()};
    unsupported.ingest(sample(MetricId::LongTermFuelTrim1, 18.0, start, SampleQuality::Unsupported));
    static_cast<void>(unsupported.evaluate(start));
    REQUIRE(unsupported.findings().empty());
}

TEST_CASE("Catalyst rule runs only with explicit meaningful O2 topology and steady warm data", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{30s};
    auto feedOxygen = [&](DiagnosticRuleEvaluator& evaluator, bool mirrored) {
        for (int second = 0; second <= 2; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(evaluator, now, 2'000.0, 70.0, 45.0, 90.0, 20.0, 0.0, 13.8);
            const auto upstream = second % 2 == 0 ? 0.10 : 0.90;
            const auto downstream = mirrored ? upstream : (second % 2 == 0 ? 0.45 : 0.50);
            evaluator.ingest(sample(MetricId::O2Sensor1Voltage, upstream, now));
            evaluator.ingest(sample(MetricId::O2Sensor2Voltage, downstream, now));
            static_cast<void>(evaluator.evaluate(now));
        }
    };

    SECTION("supported mirrored sensors produce an advisory with evidence") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        evaluator.setOxygenSensorTopology(revdash::diagnostics::OxygenSensorTopology{});
        feedOxygen(evaluator, true);
        const auto result = finding(evaluator, "HEURISTIC_CATALYST_EFFICIENCY");
        REQUIRE(result.has_value());
        REQUIRE(result->active);
        REQUIRE(result->evidence.size() == 3);
    }

    SECTION("no topology is unsupported and damped downstream data is negative") {
        DiagnosticRuleEvaluator unsupported{fastConfig()};
        feedOxygen(unsupported, true);
        REQUIRE_FALSE(finding(unsupported, "HEURISTIC_CATALYST_EFFICIENCY").has_value());

        DiagnosticRuleEvaluator healthy{fastConfig()};
        healthy.setOxygenSensorTopology(revdash::diagnostics::OxygenSensorTopology{});
        feedOxygen(healthy, false);
        REQUIRE_FALSE(finding(healthy, "HEURISTIC_CATALYST_EFFICIENCY").has_value());
    }
}

TEST_CASE("Thermostat rule evaluates validated cold-start warmup behavior", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{40s};

    SECTION("insufficient warmup triggers advisory") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        for (int second = 0; second <= 3; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(evaluator, now, 1'200.0, 0.0, 25.0, 20.0 + second * 10.0, 20.0, 0.0, 13.5);
            static_cast<void>(evaluator.evaluate(now));
        }
        const auto result = finding(evaluator, "HEURISTIC_THERMOSTAT_STUCK_OPEN");
        REQUIRE(result.has_value());
        REQUIRE(result->active);
        REQUIRE(result->evidence.size() == 3);
    }

    SECTION("normal warmup and a non-cold initial observation do not trigger") {
        DiagnosticRuleEvaluator normal{fastConfig()};
        for (int second = 0; second <= 3; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(normal, now, 1'200.0, 0.0, 25.0, 20.0 + second * 22.0, 20.0, 0.0, 13.5);
            static_cast<void>(normal.evaluate(now));
        }
        REQUIRE_FALSE(finding(normal, "HEURISTIC_THERMOSTAT_STUCK_OPEN").has_value());

        DiagnosticRuleEvaluator not_cold{fastConfig()};
        for (int second = 0; second <= 4; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(not_cold, now, 1'200.0, 0.0, 25.0, 60.0, 20.0, 0.0, 13.5);
            static_cast<void>(not_cold.evaluate(now));
        }
        REQUIRE_FALSE(finding(not_cold, "HEURISTIC_THERMOSTAT_STUCK_OPEN").has_value());
    }
}

TEST_CASE("Charging rule uses conservative sustained limits", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{50s};

    SECTION("sustained low voltage triggers only an advisory") {
        DiagnosticRuleEvaluator evaluator{fastConfig()};
        for (int second = 0; second <= 2; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(evaluator, now, 1'500.0, 40.0, 35.0, 85.0, 20.0, 0.0, 11.4);
            static_cast<void>(evaluator.evaluate(now));
        }
        const auto result = finding(evaluator, "HEURISTIC_CHARGING_VOLTAGE");
        REQUIRE(result.has_value());
        REQUIRE(result->severity == revdash::core::Severity::Advisory);
    }

    SECTION("smart-charging-like voltage and insufficient duration are negative") {
        DiagnosticRuleEvaluator smart{fastConfig()};
        for (int second = 0; second <= 2; ++second) {
            const auto now = start + std::chrono::seconds{second};
            ingest(smart, now, 1'500.0, 40.0, 35.0, 85.0, 20.0, 0.0, second == 1 ? 15.2 : 12.2);
            static_cast<void>(smart.evaluate(now));
        }
        REQUIRE_FALSE(finding(smart, "HEURISTIC_CHARGING_VOLTAGE").has_value());

        DiagnosticRuleEvaluator short_window{fastConfig()};
        ingest(short_window, start, 1'500.0, 40.0, 35.0, 85.0, 20.0, 0.0, 10.0);
        static_cast<void>(short_window.evaluate(start));
        REQUIRE_FALSE(finding(short_window, "HEURISTIC_CHARGING_VOLTAGE").has_value());
    }
}

TEST_CASE("Findings require stable clear data to resolve and epoch changes reset state", "[diagnostic_rules]") {
    const auto start = MonotonicTimePoint{60s};
    DiagnosticRuleEvaluator evaluator{fastConfig()};
    evaluator.setEpoch(7);
    addVacuumLeakSequence(evaluator, start, 18.0);
    REQUIRE(finding(evaluator, "HEURISTIC_VACUUM_LEAK")->active);

    for (int second = 6; second <= 10; ++second) {
        const auto now = start + std::chrono::seconds{second};
        ingest(evaluator, now, 900.0, 0.0, 20.0, 85.0, 20.0, 2.0, 13.5);
        static_cast<void>(evaluator.evaluate(now));
        if (second < 10) REQUIRE(finding(evaluator, "HEURISTIC_VACUUM_LEAK")->active);
    }
    const auto resolved = finding(evaluator, "HEURISTIC_VACUUM_LEAK");
    REQUIRE_FALSE(resolved->active);
    REQUIRE(resolved->resolved_at.has_value());
    REQUIRE(resolved->last_seen < *resolved->resolved_at);

    evaluator.setEpoch(8);
    REQUIRE(evaluator.epoch() == 8);
    REQUIRE(evaluator.findings().empty());
}
