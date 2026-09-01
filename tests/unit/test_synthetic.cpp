#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "revdash/drivers/synthetic.hpp"

using namespace revdash;

namespace {

template <typename Predicate>
bool waitFor(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void connect(drivers::SyntheticDataSource& source, core::SyntheticConfig config = {}) {
    std::atomic<bool> complete{false};
    source.connect(config, [&complete](core::Result<void> result) { REQUIRE(result.has_value()); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
}

} // namespace

TEST_CASE("Synthetic powertrain is deterministic and fixed-step", "[synthetic_physics]") {
    drivers::SimulationConfig config{.deterministic_seed = 77};
    drivers::SyntheticPowertrain once{config};
    drivers::SyntheticPowertrain split{config};
    once.setThrottle(35.0);
    split.setThrottle(35.0);
    once.advance(std::chrono::seconds{10});
    for (int i = 0; i < 1000; ++i) split.advance(std::chrono::milliseconds{10});

    REQUIRE(once.trueState().rpm == Catch::Approx(split.trueState().rpm));
    REQUIRE(once.trueState().vehicle_speed_kph == Catch::Approx(split.trueState().vehicle_speed_kph));
    REQUIRE(once.trueState().coolant_temp_c == Catch::Approx(split.trueState().coolant_temp_c));
    REQUIRE(once.trueState().rpm > config.idle_rpm);
    REQUIRE(once.trueState().vehicle_speed_kph > 0.0);

    drivers::SyntheticPowertrain idle{config};
    idle.advance(std::chrono::seconds{5});
    REQUIRE(idle.trueState().rpm == Catch::Approx(config.idle_rpm).margin(200.0));
    idle.setThrottle(100.0);
    idle.advance(std::chrono::minutes{2});
    REQUIRE(idle.trueState().rpm <= config.redline_rpm);
    REQUIRE(idle.trueState().map_kpa > 25.0);
    REQUIRE(idle.trueState().maf_g_per_s > 1.0);

    drivers::SyntheticPowertrain cold{config};
    drivers::SyntheticPowertrain thermostat_fault{config};
    thermostat_fault.setFaults({.stuck_open_thermostat = true});
    cold.advance(std::chrono::minutes{5});
    thermostat_fault.advance(std::chrono::minutes{5});
    REQUIRE(cold.trueState().coolant_temp_c > config.ambient_temp_c);
    REQUIRE(thermostat_fault.trueState().coolant_temp_c < cold.trueState().coolant_temp_c);
}

TEST_CASE("Synthetic faults and noise preserve deterministic physical state", "[synthetic_faults]") {
    drivers::SyntheticPowertrain base{drivers::SimulationConfig{.deterministic_seed = 9}};
    drivers::SyntheticPowertrain noisy{drivers::SimulationConfig{.deterministic_seed = 9}};
    drivers::SimulationFaultConfig faults{.misfire = true, .misfire_cylinder = 2, .vacuum_leak = true, .stuck_open_thermostat = true, .sensor_noise_std_dev = 0.5, .packet_dropout_probability = 0.25};
    base.setFaults(faults);
    noisy.setFaults(faults);
    base.advance(std::chrono::seconds{30});
    noisy.advance(std::chrono::seconds{30});
    REQUIRE(base.trueState().rpm == Catch::Approx(noisy.trueState().rpm));
    REQUIRE(base.storedDtcs().size() == 3);
    REQUIRE(base.storedDtcs().front().code == "P0302");
    REQUIRE(base.freezeFrame().has_value());
    REQUIRE(base.trueState().long_term_fuel_trim_percent > 0.0);
    REQUIRE(base.sensorState().rpm != Catch::Approx(base.trueState().rpm));
    REQUIRE(base.shouldDropPacket() == noisy.shouldDropPacket());
    base.clearDiagnosticInformation();
    REQUIRE(base.storedDtcs().empty());
    REQUIRE_FALSE(base.freezeFrame().has_value());
}

TEST_CASE("Synthetic source supplies canonical OBD responses and controls", "[synthetic_source]") {
    drivers::SyntheticDataSource source;
    connect(source, core::SyntheticConfig{.deterministic_seed = 5, .inject_misfire = true, .response_latency = std::chrono::milliseconds{10}, .include_second_ecu = true});
    std::vector<core::ObdMessage> received;
    auto subscription = source.subscribe([&received](const core::ObdMessage& message) { received.push_back(message); }, {});
    std::atomic<bool> complete{false};
    std::atomic<bool> success{false};
    source.transmit({.mode = 0x01, .pid = 0x0C}, [&complete, &success](core::Result<void> result) { success = result.has_value(); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
    REQUIRE(success.load());
    REQUIRE(received.size() == 1);
    REQUIRE(received.front().payload()[0] == 0x41);
    REQUIRE(received.front().payload()[1] == 0x0C);
    REQUIRE(received.front().ecu_address == core::EcuAddress{0x7E8});

    complete = false;
    source.transmit({.mode = 0x03}, [&complete, &success](core::Result<void> result) { success = result.has_value(); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
    REQUIRE(success.load());
    REQUIRE(received.size() == 2);
    REQUIRE(received.back().payload()[0] == 0x43);

    complete = false;
    source.transmit({.mode = 0x04}, [&complete, &success](core::Result<void> result) { success = result.has_value(); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
    REQUIRE(success.load());
    REQUIRE(received.size() == 3);
    REQUIRE(received.back().payload()[0] == 0x44);

    complete = false;
    source.transmit({.mode = 0x09, .pid = 0x02}, [&complete, &success](core::Result<void> result) { success = result.has_value(); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
    REQUIRE(success.load());
    REQUIRE(received.size() == 5);
    REQUIRE(received.at(3).ecu_address == core::EcuAddress{0x7E8});
    REQUIRE(received.at(4).ecu_address == core::EcuAddress{0x7E9});

    complete = false;
    source.transmit({.mode = 0x01, .pid = 0x01}, [&complete, &success](core::Result<void> result) { success = result.has_value(); complete = true; });
    REQUIRE(waitFor([&] { return complete.load(); }));
    REQUIRE(success.load());
    REQUIRE(received.back().payload()[0] == 0x7F);
    REQUIRE(received.back().payload()[1] == 0x01);
}
