#include "revdash/drivers/synthetic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace revdash::drivers {
namespace {

constexpr double kAirDensityKgPerM3 = 1.225;
constexpr double kGravityMps2 = 9.80665;

[[nodiscard]] double clamp(double value, double low, double high) noexcept {
    return std::clamp(value, low, high);
}

[[nodiscard]] std::uint8_t encodePercent(double value) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp(value, 0.0, 100.0) * 255.0 / 100.0));
}

[[nodiscard]] std::uint8_t encodeTemperature(double value) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp(value + 40.0, 0.0, 255.0)));
}

[[nodiscard]] std::uint8_t encodeFuelTrim(double percent) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp(percent * 128.0 / 100.0 + 128.0, 0.0, 255.0)));
}

[[nodiscard]] std::array<std::uint8_t, 2> encodeU16(double value, double scale) noexcept {
    const auto raw = static_cast<std::uint16_t>(std::lround(clamp(value * scale, 0.0, 65535.0)));
    return {static_cast<std::uint8_t>(raw >> 8U), static_cast<std::uint8_t>(raw & 0xFFU)};
}

[[nodiscard]] std::array<std::uint8_t, 2> encodeDtc(const std::string& code) {
    if (code == "P0300") return {0x03, 0x00};
    if (code == "P0301") return {0x03, 0x01};
    if (code == "P0302") return {0x03, 0x02};
    if (code == "P0303") return {0x03, 0x03};
    if (code == "P0304") return {0x03, 0x04};
    if (code == "P0171") return {0x01, 0x71};
    return {0x01, 0x28}; // P0128
}

[[nodiscard]] std::vector<std::uint8_t> bitmapFor(std::uint8_t base) {
    std::array<std::uint8_t, 4> bytes{};
    constexpr std::array<std::uint8_t, 14> supported{
        0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x11, 0x14, 0x2F, 0x42, 0x46
    };
    for (const auto pid : supported) {
        if (pid <= base || pid > static_cast<std::uint16_t>(base) + 32U) continue;
        const auto bit = static_cast<std::size_t>(pid - base - 1U);
        bytes[bit / 8U] |= static_cast<std::uint8_t>(0x80U >> (bit % 8U));
    }
    return {bytes.begin(), bytes.end()};
}

} // namespace

SyntheticPowertrain::SyntheticPowertrain(SimulationConfig config)
    : config_(std::move(config)), random_(config_.deterministic_seed) {
    reset();
}

void SyntheticPowertrain::reset() {
    random_.seed(config_.deterministic_seed);
    state_ = PowertrainState{
        .rpm = clamp(config_.initial_rpm, 0.0, config_.redline_rpm),
        .coolant_temp_c = config_.ambient_temp_c,
    };
    accumulator_ms_ = 0.0;
    idle_integral_ = 0.0;
    step_count_ = 0;
    freeze_frame_.reset();
}

void SyntheticPowertrain::advance(std::chrono::milliseconds elapsed) {
    accumulator_ms_ += static_cast<double>(elapsed.count());
    while (accumulator_ms_ >= static_cast<double>(kPhysicsStep.count())) {
        step();
        accumulator_ms_ -= static_cast<double>(kPhysicsStep.count());
    }
}

void SyntheticPowertrain::setThrottle(double percent) noexcept { state_.throttle_percent = clamp(percent, 0.0, 100.0); }
void SyntheticPowertrain::setAmbientTemperature(double celsius) noexcept { config_.ambient_temp_c = clamp(celsius, -40.0, 80.0); }
void SyntheticPowertrain::setFaults(SimulationFaultConfig faults) noexcept { faults_ = faults; faults_.sensor_noise_std_dev = std::max(0.0, faults_.sensor_noise_std_dev); faults_.packet_dropout_probability = clamp(faults_.packet_dropout_probability, 0.0, 1.0); }
const SimulationConfig& SyntheticPowertrain::config() const noexcept { return config_; }
const PowertrainState& SyntheticPowertrain::trueState() const noexcept { return state_; }

void SyntheticPowertrain::step() {
    constexpr double dt = 0.01;
    const auto throttle = state_.throttle_percent / 100.0;
    const auto speed_mps = state_.vehicle_speed_kph / 3.6;
    const auto normalized_rpm = clamp(state_.rpm / config_.redline_rpm, 0.0, 1.0);
    const auto torque_shape = std::max(0.25, 1.0 - std::pow(normalized_rpm - 0.50, 2.0) * 2.0);
    auto engine_torque = config_.peak_torque_nm * throttle * torque_shape;
    if (faults_.misfire) {
        engine_torque *= 0.72;
        engine_torque += std::sin(static_cast<double>(step_count_) * 0.79) * 15.0;
    }
    if (throttle < 0.08) {
        const auto error = config_.idle_rpm - state_.rpm;
        idle_integral_ = clamp(idle_integral_ + error * dt, -300.0, 300.0);
        engine_torque += clamp(error * 0.10 + idle_integral_ * 0.020, 0.0, 75.0);
    } else {
        idle_integral_ *= 0.98;
    }
    if (state_.rpm >= config_.redline_rpm) engine_torque = -90.0;

    const auto pumping_loss = 12.0 + state_.rpm * 0.006;
    state_.rpm = clamp(state_.rpm + ((engine_torque - pumping_loss) / config_.engine_inertia_kg_m2) * dt * 2.3, 550.0, config_.redline_rpm);
    const auto wheel_force = std::max(0.0, engine_torque) * config_.final_drive_ratio / config_.wheel_radius_m;
    const auto drag = 0.5 * kAirDensityKgPerM3 * config_.drag_coefficient * config_.frontal_area_m2 * speed_mps * speed_mps;
    const auto rolling = config_.rolling_resistance * config_.vehicle_mass_kg * kGravityMps2;
    const auto acceleration = (wheel_force - drag - rolling) / config_.vehicle_mass_kg;
    state_.vehicle_speed_kph = clamp(state_.vehicle_speed_kph + acceleration * dt * 3.6, 0.0, 255.0);

    const auto load = clamp(throttle * 0.75 + normalized_rpm * 0.25, 0.0, 1.0);
    state_.map_kpa = clamp(25.0 + load * 72.0 + (faults_.vacuum_leak ? 8.0 * (1.0 - throttle) : 0.0), 15.0, 101.0);
    const auto volumetric_efficiency = 0.70 + 0.18 * load;
    state_.maf_g_per_s = std::max(1.2, config_.displacement_liters * state_.rpm * state_.map_kpa * volumetric_efficiency / 12000.0);
    state_.short_term_fuel_trim_percent = faults_.vacuum_leak ? 20.0 * (1.0 - throttle) : 0.0;
    state_.long_term_fuel_trim_percent = faults_.vacuum_leak ? 15.0 * (1.0 - throttle) : 0.0;
    state_.timing_advance_deg = clamp(8.0 + normalized_rpm * 22.0 - throttle * 8.0, -10.0, 35.0);
    const auto target_coolant = faults_.stuck_open_thermostat ? 65.0 : 92.0;
    const auto heat = 0.004 * state_.rpm * (0.20 + throttle) * dt;
    const auto thermostat = state_.coolant_temp_c > target_coolant ? (state_.coolant_temp_c - target_coolant) * 0.07 * dt : 0.0;
    const auto fan = state_.coolant_temp_c > 98.0 ? (state_.coolant_temp_c - 96.0) * 0.18 * dt : 0.0;
    state_.coolant_temp_c += heat - thermostat - fan - (state_.coolant_temp_c - config_.ambient_temp_c) * 0.002 * dt;
    state_.fuel_level_percent = std::max(0.0, state_.fuel_level_percent - state_.maf_g_per_s * dt / 55000.0);
    ++step_count_;
    captureFreezeFrameIfNeeded();
}

void SyntheticPowertrain::captureFreezeFrameIfNeeded() {
    if (!faults_.misfire || freeze_frame_.has_value()) return;
    const auto records = storedDtcs();
    if (records.empty()) return;
    freeze_frame_ = core::FreezeFrame{.dtc_code = records.front().code, .frame_number = 0, .timestamp = core::MonotonicClock::now(), .samples = {
        {.metric_id = core::MetricId::Rpm, .value = state_.rpm, .quality = core::SampleQuality::Valid},
        {.metric_id = core::MetricId::CoolantTemp, .value = state_.coolant_temp_c, .quality = core::SampleQuality::Valid},
        {.metric_id = core::MetricId::EngineLoad, .value = state_.throttle_percent, .quality = core::SampleQuality::Valid}
    }};
}

PowertrainState SyntheticPowertrain::sensorState() {
    auto measured = state_;
    if (faults_.sensor_noise_std_dev <= 0.0) return measured;
    std::normal_distribution<double> noise{0.0, faults_.sensor_noise_std_dev};
    measured.rpm = std::max(0.0, measured.rpm + noise(random_));
    measured.map_kpa = std::max(0.0, measured.map_kpa + noise(random_));
    measured.maf_g_per_s = std::max(0.0, measured.maf_g_per_s + noise(random_));
    measured.coolant_temp_c += noise(random_);
    return measured;
}

bool SyntheticPowertrain::shouldDropPacket() {
    return std::bernoulli_distribution{faults_.packet_dropout_probability}(random_);
}

std::vector<core::DtcRecord> SyntheticPowertrain::storedDtcs() const {
    std::vector<core::DtcRecord> records;
    if (faults_.misfire) records.push_back({.code = faults_.misfire_cylinder >= 1 && faults_.misfire_cylinder <= 4 ? "P030" + std::to_string(faults_.misfire_cylinder) : "P0300", .status = core::DtcStatus::Confirmed, .severity = core::Severity::Critical});
    if (faults_.vacuum_leak) records.push_back({.code = "P0171", .status = core::DtcStatus::Confirmed, .severity = core::Severity::Warning});
    if (faults_.stuck_open_thermostat) records.push_back({.code = "P0128", .status = core::DtcStatus::Confirmed, .severity = core::Severity::Warning});
    return records;
}

std::vector<core::DtcRecord> SyntheticPowertrain::pendingDtcs() const {
    auto records = storedDtcs();
    for (auto& record : records) record.status = core::DtcStatus::Pending;
    return records;
}

std::optional<core::FreezeFrame> SyntheticPowertrain::freezeFrame() const { return freeze_frame_; }
void SyntheticPowertrain::clearDiagnosticInformation() { faults_.misfire = false; faults_.vacuum_leak = false; faults_.stuck_open_thermostat = false; freeze_frame_.reset(); }

SyntheticDataSource::SyntheticDataSource() : AsyncDataSource(core::DataSourceType::Synthetic) {}

void SyntheticDataSource::setThrottle(double percent) { postToWorker([this, percent] { powertrain_.setThrottle(percent); }); }
void SyntheticDataSource::setAmbientTemperature(double celsius) { postToWorker([this, celsius] { powertrain_.setAmbientTemperature(celsius); }); }
void SyntheticDataSource::setFaults(SimulationFaultConfig faults) { postToWorker([this, faults] { powertrain_.setFaults(faults); }); }
void SyntheticDataSource::resetSimulation() { postToWorker([this] { powertrain_.reset(); sequence_ = 0; }); }

void SyntheticDataSource::startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) {
    const auto* synthetic = std::get_if<core::SyntheticConfig>(&config);
    if (synthetic == nullptr) { completion(core::makeError(core::ErrorCode::CoreInvalidState, "Synthetic source requires SyntheticConfig")); return; }
    source_config_ = *synthetic;
    powertrain_ = SyntheticPowertrain{SimulationConfig{.displacement_liters = synthetic->displacement_liters, .cylinder_count = synthetic->cylinder_count, .initial_rpm = synthetic->initial_rpm, .ambient_temp_c = synthetic->ambient_temp_c, .deterministic_seed = synthetic->deterministic_seed}};
    powertrain_.setFaults({.misfire = synthetic->inject_misfire, .vacuum_leak = synthetic->inject_vacuum_leak, .stuck_open_thermostat = synthetic->inject_thermostat_fault, .sensor_noise_std_dev = synthetic->noise_std_dev, .packet_dropout_probability = synthetic->packet_dropout_prob});
    completion(core::makeSuccess());
}

void SyntheticDataSource::startDisconnect(core::CompletionCallback completion) { completion(core::makeSuccess()); }

void SyntheticDataSource::startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) {
    postAfterToWorker(source_config_.response_latency, [this, request, completion = std::move(completion)]() mutable {
        powertrain_.advance(SyntheticPowertrain::kPhysicsStep);
        auto response = respond(request);
        if (!response.has_value()) { completion(tl::make_unexpected(response.error())); return; }
        if (!powertrain_.shouldDropPacket()) for (const auto& message : *response) publishMessage(message);
        completion(core::makeSuccess());
    });
}

core::Result<core::ObdMessage> SyntheticDataSource::makeMessage(std::vector<std::uint8_t> payload, std::optional<core::EcuAddress> ecu) {
    return core::ObdMessage::create(core::DataSourceType::Synthetic, ecu, payload, ++sequence_);
}

core::Result<std::vector<core::ObdMessage>> SyntheticDataSource::respond(const core::ObdRequest& request) {
    const auto state = powertrain_.sensorState();
    const auto first_ecu = request.target_ecu.value_or(core::EcuAddress{0x7E8});
    std::vector<core::ObdMessage> messages;
    auto append = [&](std::vector<std::uint8_t> payload, core::EcuAddress ecu) -> core::Result<void> { auto message = makeMessage(std::move(payload), ecu); if (!message) return tl::make_unexpected(message.error()); messages.push_back(*message); return {}; };
    if (request.mode == 0x01) {
        std::vector<std::uint8_t> payload{0x41, request.pid};
        switch (request.pid) {
            case 0x00: { const auto bitmap = bitmapFor(0x00); payload.insert(payload.end(), bitmap.begin(), bitmap.end()); break; }
            case 0x20: { const auto bitmap = bitmapFor(0x20); payload.insert(payload.end(), bitmap.begin(), bitmap.end()); break; }
            case 0x04: payload.push_back(encodePercent(state.throttle_percent)); break;
            case 0x05: payload.push_back(encodeTemperature(state.coolant_temp_c)); break;
            case 0x06: payload.push_back(encodeFuelTrim(state.short_term_fuel_trim_percent)); break;
            case 0x07: payload.push_back(encodeFuelTrim(state.long_term_fuel_trim_percent)); break;
            case 0x0B: payload.push_back(static_cast<std::uint8_t>(std::lround(state.map_kpa))); break;
            case 0x0C: { const auto bytes = encodeU16(state.rpm, 4.0); payload.insert(payload.end(), bytes.begin(), bytes.end()); break; }
            case 0x0D: payload.push_back(static_cast<std::uint8_t>(std::lround(state.vehicle_speed_kph))); break;
            case 0x0E: payload.push_back(static_cast<std::uint8_t>(std::lround((state.timing_advance_deg + 64.0) * 2.0))); break;
            case 0x11: payload.push_back(encodePercent(state.throttle_percent)); break;
            case 0x2F: payload.push_back(encodePercent(state.fuel_level_percent)); break;
            case 0x42: { const auto bytes = encodeU16(state.module_voltage, 1000.0); payload.insert(payload.end(), bytes.begin(), bytes.end()); break; }
            case 0x46: payload.push_back(encodeTemperature(powertrain_.config().ambient_temp_c)); break;
            default: payload = {0x7F, 0x01, 0x11}; break;
        }
        if (auto result = append(std::move(payload), first_ecu); !result) return tl::make_unexpected(result.error());
    } else if (request.mode == 0x02) {
        if (request.pid != 0x0C) {
            if (auto result = append({0x7F, 0x02, 0x11}, first_ecu); !result) return tl::make_unexpected(result.error());
        } else {
            const auto bytes = encodeU16(state.rpm, 4.0);
            if (auto result = append({0x42, 0x0C, bytes[0], bytes[1]}, first_ecu); !result) return tl::make_unexpected(result.error());
        }
    } else if (request.mode == 0x03 || request.mode == 0x07) {
        std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(request.mode + 0x40)};
        const auto records = request.mode == 0x03 ? powertrain_.storedDtcs() : powertrain_.pendingDtcs();
        for (const auto& record : records) { const auto bytes = encodeDtc(record.code); payload.insert(payload.end(), bytes.begin(), bytes.end()); }
        if (auto result = append(std::move(payload), first_ecu); !result) return tl::make_unexpected(result.error());
    } else if (request.mode == 0x04) {
        powertrain_.clearDiagnosticInformation();
        if (auto result = append({0x44}, first_ecu); !result) return tl::make_unexpected(result.error());
    } else if (request.mode == 0x09) {
        if (request.pid == 0x02) {
            if (auto result = append({0x49, 0x02, 0x01, '1', 'H', 'G', 'C', 'R', '2', 'F', '8', '3', 'H', 'A', '0', '0', '0', '0', '0', '0'}, first_ecu); !result) return tl::make_unexpected(result.error());
        } else if (request.pid == 0x04) {
            if (auto result = append({0x49, 0x04, 0x01, 'S', 'Y', 'N', 'T', 'H', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '1'}, first_ecu); !result) return tl::make_unexpected(result.error());
        } else if (request.pid == 0x06) {
            if (auto result = append({0x49, 0x06, 0x01, 0x53, 0x59, 0x4E, 0x31}, first_ecu); !result) return tl::make_unexpected(result.error());
        } else {
            if (auto result = append({0x7F, 0x09, 0x11}, first_ecu); !result) return tl::make_unexpected(result.error());
            return messages;
        }
        if (source_config_.include_second_ecu && !request.target_ecu) {
            std::vector<std::uint8_t> second_payload;
            if (request.pid == 0x02) second_payload = {0x49, 0x02, 0x01, '2', 'H', 'G', 'C', 'R', '2', 'F', '8', '3', 'H', 'A', '0', '0', '0', '0', '0', '1'};
            if (request.pid == 0x04) second_payload = {0x49, 0x04, 0x01, 'S', 'Y', 'N', 'T', 'H', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '2'};
            if (request.pid == 0x06) second_payload = {0x49, 0x06, 0x01, 0x53, 0x59, 0x4E, 0x32};
            if (auto result = append(std::move(second_payload), core::EcuAddress{0x7E9}); !result) return tl::make_unexpected(result.error());
        }
    } else {
        if (auto result = append({0x7F, request.mode, 0x11}, first_ecu); !result) return tl::make_unexpected(result.error());
    }
    return messages;
}

} // namespace revdash::drivers
