#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "revdash/core/async_data_source.hpp"
#include "revdash/core/diagnostic_types.hpp"
#include "revdash/core/telemetry_types.hpp"

namespace revdash::drivers {

struct SimulationConfig {
    double displacement_liters{2.0};
    std::uint32_t cylinder_count{4};
    double initial_rpm{800.0};
    double idle_rpm{800.0};
    double redline_rpm{6500.0};
    double peak_torque_nm{220.0};
    double engine_inertia_kg_m2{0.24};
    double vehicle_mass_kg{1450.0};
    double drag_coefficient{0.31};
    double frontal_area_m2{2.2};
    double rolling_resistance{0.012};
    double wheel_radius_m{0.31};
    double final_drive_ratio{4.1};
    double ambient_temp_c{20.0};
    std::uint32_t deterministic_seed{12345};
};

struct SimulationFaultConfig {
    bool misfire{false};
    std::uint8_t misfire_cylinder{0}; // Zero means random/multiple-cylinder P0300.
    bool vacuum_leak{false};
    bool stuck_open_thermostat{false};
    double sensor_noise_std_dev{0.0};
    double packet_dropout_probability{0.0};
};

struct PowertrainState {
    double rpm{0.0};
    double vehicle_speed_kph{0.0};
    double throttle_percent{0.0};
    double map_kpa{0.0};
    double maf_g_per_s{0.0};
    double coolant_temp_c{0.0};
    double short_term_fuel_trim_percent{0.0};
    double long_term_fuel_trim_percent{0.0};
    double timing_advance_deg{0.0};
    double fuel_level_percent{75.0};
    double module_voltage{14.1};
};

class SyntheticPowertrain {
public:
    static constexpr auto kPhysicsStep = std::chrono::milliseconds{10};

    explicit SyntheticPowertrain(SimulationConfig config = {});

    void reset();
    void advance(std::chrono::milliseconds elapsed);
    void setThrottle(double percent) noexcept;
    void setAmbientTemperature(double celsius) noexcept;
    void setFaults(SimulationFaultConfig faults) noexcept;

    [[nodiscard]] const SimulationConfig& config() const noexcept;
    [[nodiscard]] const PowertrainState& trueState() const noexcept;
    [[nodiscard]] PowertrainState sensorState();
    [[nodiscard]] bool shouldDropPacket();
    [[nodiscard]] std::vector<core::DtcRecord> storedDtcs() const;
    [[nodiscard]] std::vector<core::DtcRecord> pendingDtcs() const;
    [[nodiscard]] std::optional<core::FreezeFrame> freezeFrame() const;
    void clearDiagnosticInformation();

private:
    void step();
    void captureFreezeFrameIfNeeded();

    SimulationConfig config_;
    SimulationFaultConfig faults_;
    PowertrainState state_;
    double accumulator_ms_{0.0};
    double idle_integral_{0.0};
    std::uint64_t step_count_{0};
    std::mt19937 random_;
    std::optional<core::FreezeFrame> freeze_frame_;
};

class SyntheticDataSource final : public core::AsyncDataSource {
public:
    SyntheticDataSource();

    void setThrottle(double percent);
    void setAmbientTemperature(double celsius);
    void setFaults(SimulationFaultConfig faults);
    void resetSimulation();

protected:
    void startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) override;
    void startDisconnect(core::CompletionCallback completion) override;
    void startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) override;

private:
    [[nodiscard]] core::Result<std::vector<core::ObdMessage>> respond(const core::ObdRequest& request);
    [[nodiscard]] core::Result<core::ObdMessage> makeMessage(std::vector<std::uint8_t> payload, std::optional<core::EcuAddress> ecu);

    SyntheticPowertrain powertrain_;
    core::SyntheticConfig source_config_;
    std::uint64_t sequence_{0};
};

} // namespace revdash::drivers
