#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <unordered_set>
#include <vector>
#include "revdash/core/error.hpp"
#include "revdash/core/types.hpp"
#include "revdash/core/telemetry_types.hpp"
#include "revdash/core/diagnostic_types.hpp"

using namespace revdash::core;

TEST_CASE("Core version and identity validation", "[core_types]") {
    REQUIRE(kApplicationName == "RevDash");
    REQUIRE(kApplicationVersion == "0.1.0");
}

TEST_CASE("Error and Result domain models", "[core_types]") {
    SECTION("Success result") {
        Result<int> res = 42;
        REQUIRE(res.has_value());
        REQUIRE(*res == 42);
    }

    SECTION("Error result creation and formatting") {
        Result<int> res = makeError(
            ErrorDomain::Transport,
            "Transport.Timeout",
            "Serial read operation timed out",
            true,
            "COM3 @ 38400 baud"
        );

        REQUIRE_FALSE(res.has_value());
        const auto& err = res.error();
        REQUIRE(err.domain == ErrorDomain::Transport);
        REQUIRE(toString(err.domain) == "Transport");
        REQUIRE(err.code == "Transport.Timeout");
        REQUIRE(err.message == "Serial read operation timed out");
        REQUIRE(err.retryable == true);
        REQUIRE(err.context == "COM3 @ 38400 baud");
    }

    SECTION("All ErrorDomain toString mappings") {
        REQUIRE(toString(ErrorDomain::Core) == "Core");
        REQUIRE(toString(ErrorDomain::Transport) == "Transport");
        REQUIRE(toString(ErrorDomain::Protocol) == "Protocol");
        REQUIRE(toString(ErrorDomain::Diagnostics) == "Diagnostics");
        REQUIRE(toString(ErrorDomain::Session) == "Session");
        REQUIRE(toString(ErrorDomain::Storage) == "Storage");
    }

    SECTION("Stable ErrorCode mappings retain their domain-qualified values") {
        REQUIRE(errorDomain(ErrorCode::CoreCancelled) == ErrorDomain::Core);
        REQUIRE(toString(ErrorCode::CoreCancelled) == "Core.Cancelled");
        REQUIRE(errorDomain(ErrorCode::TransportTimeout) == ErrorDomain::Transport);
        REQUIRE(toString(ErrorCode::TransportTimeout) == "Transport.Timeout");
        REQUIRE(errorDomain(ErrorCode::ProtocolPayloadTooLarge) == ErrorDomain::Protocol);
        REQUIRE(toString(ErrorCode::ProtocolPayloadTooLarge) == "Protocol.PayloadTooLarge");
        REQUIRE(errorDomain(ErrorCode::DiagnosticsUnsupported) == ErrorDomain::Diagnostics);
        REQUIRE(errorDomain(ErrorCode::SessionInvalidFormat) == ErrorDomain::Session);
        REQUIRE(errorDomain(ErrorCode::StorageUnavailable) == ErrorDomain::Storage);

        Result<void> result = makeError(ErrorCode::CoreUnsupportedPlatform, "SocketCAN is unavailable");
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == "Core.UnsupportedPlatform");
        REQUIRE(result.error().domain == ErrorDomain::Core);
    }
}

TEST_CASE("ConnectionState lifecycle and transitions", "[core_types]") {
    REQUIRE(toString(ConnectionState::Disconnected) == "Disconnected");
    REQUIRE(toString(ConnectionState::Connecting) == "Connecting");
    REQUIRE(toString(ConnectionState::Initializing) == "Initializing");
    REQUIRE(toString(ConnectionState::Ready) == "Ready");
    REQUIRE(toString(ConnectionState::Reconnecting) == "Reconnecting");
    REQUIRE(toString(ConnectionState::Disconnecting) == "Disconnecting");
    REQUIRE(toString(ConnectionState::Faulted) == "Faulted");
}

TEST_CASE("RawTransportFrame and ObdRequest models", "[core_types]") {
    SECTION("RawTransportFrame initialization and payload span") {
        RawTransportFrame frame;
        frame.source_type = DataSourceType::SerialElm327;
        frame.ecu_address = 0x7E8;
        frame.sequence_number = 100;
        frame.data[0] = 0x41;
        frame.data[1] = 0x0C;
        frame.data[2] = 0x1A;
        frame.data[3] = 0xF8;
        frame.length = 4;

        REQUIRE(frame.payload().size() == 4);
        REQUIRE(frame.payload()[0] == 0x41);
        REQUIRE(frame.payload()[3] == 0xF8);
    }

    SECTION("ObdRequest creation") {
        ObdRequest req{
            .mode = 0x01,
            .pid = 0x0C,
            .target_ecu = 0x7E0,
            .extra_length = 0
        };

        REQUIRE(req.mode == 0x01);
        REQUIRE(req.pid == 0x0C);
        REQUIRE(req.target_ecu.has_value());
        REQUIRE(*req.target_ecu == EcuAddress{0x7E0});
        REQUIRE(req.extra_payload().empty());
    }
}

TEST_CASE("EcuAddress preserves format and supports hashing", "[core_types]") {
    const EcuAddress can11{0x7E8, EcuAddressFormat::Can11Bit};
    const EcuAddress same_can11{0x7E8, EcuAddressFormat::Can11Bit};
    const EcuAddress can29{0x7E8, EcuAddressFormat::Can29Bit};

    REQUIRE(can11 == same_can11);
    REQUIRE_FALSE(can11 == can29);
    REQUIRE(EcuAddressHash{}(can11) == EcuAddressHash{}(same_can11));

    std::unordered_set<EcuAddress> addresses{can11, same_can11, can29};
    REQUIRE(addresses.size() == 2);
}

TEST_CASE("ObdMessage capacity and 4095-byte payload limits", "[core_types]") {
    SECTION("Valid message below limit") {
        std::vector<std::uint8_t> bytes = {0x41, 0x0C, 0x1A, 0xF8};
        auto res = ObdMessage::create(DataSourceType::Synthetic, 0x7E8, bytes, 1);
        REQUIRE(res.has_value());
        REQUIRE(res->length == 4);
        REQUIRE(res->payload().size() == 4);
        REQUIRE(res->payload()[2] == 0x1A);
    }

    SECTION("Valid message exactly at maximum 4095-byte limit") {
        std::vector<std::uint8_t> max_bytes(4095, 0xAA);
        auto res = ObdMessage::create(DataSourceType::Synthetic, 0x7E8, max_bytes, 2);
        REQUIRE(res.has_value());
        REQUIRE(res->length == 4095);
        REQUIRE(res->payload().size() == 4095);
        REQUIRE(res->payload()[0] == 0xAA);
        REQUIRE(res->payload()[4094] == 0xAA);
    }

    SECTION("Reject oversized message > 4095 bytes with Protocol.PayloadTooLarge") {
        std::vector<std::uint8_t> oversized_bytes(4096, 0xFF);
        auto res = ObdMessage::create(DataSourceType::Synthetic, 0x7E8, oversized_bytes, 3);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().domain == ErrorDomain::Protocol);
        REQUIRE(res.error().code == "Protocol.PayloadTooLarge");
    }
}

TEST_CASE("Telemetry domain metrics, canonical units, and quality", "[core_types]") {
    SECTION("All required core metric IDs exist and have canonical units") {
        REQUIRE(toString(MetricId::Rpm) == "RPM");
        REQUIRE(getCanonicalUnit(MetricId::Rpm) == "rpm");

        REQUIRE(toString(MetricId::VehicleSpeed) == "VehicleSpeed");
        REQUIRE(getCanonicalUnit(MetricId::VehicleSpeed) == "km/h");

        REQUIRE(toString(MetricId::ThrottlePosition) == "ThrottlePosition");
        REQUIRE(getCanonicalUnit(MetricId::ThrottlePosition) == "%");

        REQUIRE(toString(MetricId::Map) == "MAP");
        REQUIRE(getCanonicalUnit(MetricId::Map) == "kPa");

        REQUIRE(toString(MetricId::Maf) == "MAF");
        REQUIRE(getCanonicalUnit(MetricId::Maf) == "g/s");

        REQUIRE(toString(MetricId::EngineLoad) == "EngineLoad");
        REQUIRE(getCanonicalUnit(MetricId::EngineLoad) == "%");

        REQUIRE(toString(MetricId::TimingAdvance) == "TimingAdvance");
        REQUIRE(getCanonicalUnit(MetricId::TimingAdvance) == "deg");

        REQUIRE(toString(MetricId::CoolantTemp) == "CoolantTemp");
        REQUIRE(getCanonicalUnit(MetricId::CoolantTemp) == "degC");

        REQUIRE(toString(MetricId::ShortTermFuelTrim1) == "STFT1");
        REQUIRE(getCanonicalUnit(MetricId::ShortTermFuelTrim1) == "%");

        REQUIRE(toString(MetricId::LongTermFuelTrim1) == "LTFT1");
        REQUIRE(getCanonicalUnit(MetricId::LongTermFuelTrim1) == "%");

        REQUIRE(toString(MetricId::ShortTermFuelTrim2) == "STFT2");
        REQUIRE(getCanonicalUnit(MetricId::ShortTermFuelTrim2) == "%");

        REQUIRE(toString(MetricId::LongTermFuelTrim2) == "LTFT2");
        REQUIRE(getCanonicalUnit(MetricId::LongTermFuelTrim2) == "%");

        REQUIRE(toString(MetricId::AmbientAirTemp) == "AmbientAirTemp");
        REQUIRE(getCanonicalUnit(MetricId::AmbientAirTemp) == "degC");

        REQUIRE(toString(MetricId::FuelLevel) == "FuelLevel");
        REQUIRE(getCanonicalUnit(MetricId::FuelLevel) == "%");

        REQUIRE(toString(MetricId::ModuleVoltage) == "ModuleVoltage");
        REQUIRE(getCanonicalUnit(MetricId::ModuleVoltage) == "V");

        REQUIRE(toString(MetricId::O2Sensor1Voltage) == "O2Sensor1Voltage");
        REQUIRE(getCanonicalUnit(MetricId::O2Sensor1Voltage) == "V");

        REQUIRE(toString(MetricId::O2Sensor2Voltage) == "O2Sensor2Voltage");
        REQUIRE(getCanonicalUnit(MetricId::O2Sensor2Voltage) == "V");

        REQUIRE(toString(MetricId::O2Sensor1EquivalenceRatio) == "O2Sensor1EquivalenceRatio");
        REQUIRE(getCanonicalUnit(MetricId::O2Sensor1EquivalenceRatio) == "ratio");

        REQUIRE(toString(MetricId::O2Sensor2EquivalenceRatio) == "O2Sensor2EquivalenceRatio");
        REQUIRE(getCanonicalUnit(MetricId::O2Sensor2EquivalenceRatio) == "ratio");
    }

    SECTION("SampleQuality enum and toString") {
        REQUIRE(toString(SampleQuality::Valid) == "Valid");
        REQUIRE(toString(SampleQuality::Stale) == "Stale");
        REQUIRE(toString(SampleQuality::Unsupported) == "Unsupported");
        REQUIRE(toString(SampleQuality::Dropped) == "Dropped");
        REQUIRE(toString(SampleQuality::Invalid) == "Invalid");

        TelemetrySample sample{.metric_id = MetricId::Rpm, .quality = SampleQuality::Unsupported};
        REQUIRE_FALSE(sample.isValid());
        sample.quality = SampleQuality::Valid;
        REQUIRE(sample.isValid());
        sample.quality = SampleQuality::Stale;
        REQUIRE_FALSE(sample.isValid());
    }

    SECTION("Metric identifiers are unique") {
        std::unordered_set<std::string_view> metric_names;
        for (std::size_t index = 0; index < kMetricCount; ++index) {
            const auto metric = static_cast<MetricId>(index);
            REQUIRE(metric_names.insert(toString(metric)).second);
            REQUIRE_FALSE(getCanonicalUnit(metric).empty());
        }
    }

    SECTION("TelemetrySample validation helper") {
        TelemetrySample s1{
            .metric_id = MetricId::Rpm,
            .value = 1750.0,
            .quality = SampleQuality::Valid
        };
        REQUIRE(s1.isValid());

        TelemetrySample s2{
            .metric_id = MetricId::Rpm,
            .value = 1750.0,
            .quality = SampleQuality::Stale
        };
        REQUIRE_FALSE(s2.isValid());
    }
}

TEST_CASE("Canonical messages preserve timestamp semantics", "[core_types]") {
    const auto monotonic_time = MonotonicTimePoint{std::chrono::seconds{42}};
    const auto utc_time = UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    const std::array<std::uint8_t, 2> bytes{0x41, 0x0C};

    const auto timestamped = ObdMessage::create(
        DataSourceType::Synthetic,
        EcuAddress{0x7E8},
        bytes,
        7,
        monotonic_time,
        utc_time
    );
    REQUIRE(timestamped.has_value());
    REQUIRE(timestamped->monotonic_ts == monotonic_time);
    REQUIRE(timestamped->utc_ts == utc_time);
    REQUIRE(timestamped->ecu_address == EcuAddress{0x7E8});
    REQUIRE(timestamped->length == bytes.size());

    const auto without_utc = ObdMessage::create(
        DataSourceType::Playback,
        std::nullopt,
        bytes,
        8,
        monotonic_time,
        std::nullopt
    );
    REQUIRE(without_utc.has_value());
    REQUIRE_FALSE(without_utc->utc_ts.has_value());
}

TEST_CASE("ManualClock advances deterministically", "[core_types]") {
    const auto monotonic_start = MonotonicTimePoint{std::chrono::milliseconds{500}};
    const auto utc_start = UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    ManualClock clock{monotonic_start, utc_start};

    clock.advance(std::chrono::milliseconds{250});
    REQUIRE(clock.monotonicNow() == monotonic_start + std::chrono::milliseconds{250});
    REQUIRE(clock.utcNow() == utc_start + std::chrono::milliseconds{250});

    clock.setUtcTime(std::nullopt);
    REQUIRE_FALSE(clock.utcNow().has_value());
    clock.advance(std::chrono::seconds{1});
    REQUIRE(clock.monotonicNow() == monotonic_start + std::chrono::milliseconds{1250});
}

TEST_CASE("TelemetrySnapshot immutable store and queries", "[core_types]") {
    TelemetrySnapshot snapshot;
    snapshot.epoch = 1;

    REQUIRE_FALSE(snapshot.isValid(MetricId::Rpm));
    REQUIRE_FALSE(snapshot.isSupported(MetricId::Rpm));
    REQUIRE(snapshot.getValueOrDefault(MetricId::Rpm, 800.0) == 800.0);

    const auto now = MonotonicClock::now();
    snapshot.samples[static_cast<std::size_t>(MetricId::Rpm)] = TelemetrySample{
        .metric_id = MetricId::Rpm,
        .value = 2450.0,
        .quality = SampleQuality::Valid,
        .monotonic_ts = now
    };

    snapshot.samples[static_cast<std::size_t>(MetricId::CoolantTemp)] = TelemetrySample{
        .metric_id = MetricId::CoolantTemp,
        .value = 92.0,
        .quality = SampleQuality::Valid,
        .monotonic_ts = now
    };

    REQUIRE(snapshot.isValid(MetricId::Rpm));
    REQUIRE(snapshot.isSupported(MetricId::Rpm));
    REQUIRE(snapshot.getValueOrDefault(MetricId::Rpm, 0.0) == 2450.0);

    REQUIRE(snapshot.isValid(MetricId::CoolantTemp));
    REQUIRE(snapshot.getValueOrDefault(MetricId::CoolantTemp, 0.0) == 92.0);
}

TEST_CASE("Diagnostic domain models and severity", "[core_types]") {
    SECTION("Severity and DtcStatus mappings") {
        REQUIRE(toString(Severity::Advisory) == "Advisory");
        REQUIRE(toString(Severity::Warning) == "Warning");
        REQUIRE(toString(Severity::Critical) == "Critical");

        REQUIRE(toString(DtcStatus::Confirmed) == "Confirmed");
        REQUIRE(toString(DtcStatus::Pending) == "Pending");
        REQUIRE(toString(DtcStatus::Permanent) == "Permanent");
    }

    SECTION("DtcRecord with FreezeFrame") {
        FreezeFrame ff{
            .dtc_code = "P0300",
            .frame_number = 0,
            .timestamp = MonotonicClock::now(),
            .samples = {
                {.metric_id = MetricId::Rpm, .value = 2100.0, .quality = SampleQuality::Valid},
                {.metric_id = MetricId::CoolantTemp, .value = 88.0, .quality = SampleQuality::Valid}
            }
        };

        DtcRecord dtc{
            .code = "P0300",
            .status = DtcStatus::Confirmed,
            .severity = Severity::Critical,
            .description = "Random/Multiple Cylinder Misfire Detected",
            .likely_failure_points = {"Spark Plugs", "Ignition Coils", "Fuel Injectors"},
            .ecu_address = 0x7E8,
            .freeze_frame = std::move(ff)
        };

        REQUIRE(dtc.code == "P0300");
        REQUIRE(dtc.status == DtcStatus::Confirmed);
        REQUIRE(dtc.severity == Severity::Critical);
        REQUIRE(dtc.likely_failure_points.size() == 3);
        REQUIRE(dtc.freeze_frame.has_value());
        REQUIRE(dtc.freeze_frame->samples.size() == 2);

        DtcRecord dtc_same{
            .code = "P0300",
            .status = DtcStatus::Confirmed,
            .ecu_address = 0x7E8
        };
        REQUIRE(dtc == dtc_same);
    }

    SECTION("EcuMetadata VIN validation and CalIDs") {
        EcuMetadata meta{
            .ecu_address = 0x7E8,
            .vin = "1HGCR2F83HA000000",
            .calibration_ids = {"12345678ABCD"},
            .cvns = {"A1B2C3D4"},
            .protocol_name = "ISO 15765-4 (CAN 11/500)"
        };

        REQUIRE(meta.vin.length() == 17);
        REQUIRE(meta.calibration_ids.size() == 1);
        REQUIRE(meta.cvns.size() == 1);
    }

    SECTION("DiagnosticFinding heuristic rule results") {
        DiagnosticFinding finding{
            .rule_id = "HEURISTIC_VACUUM_LEAK",
            .severity = Severity::Warning,
            .title = "Possible Intake Vacuum Leak",
            .description = "Elevated positive fuel trims detected under idle that normalize under load.",
            .evidence = {"Idle LTFT: +18.2%", "Load LTFT: +2.3%"},
            .first_detected = MonotonicClock::now(),
            .last_evaluated = MonotonicClock::now(),
            .active = true
        };

        REQUIRE(finding.rule_id == "HEURISTIC_VACUUM_LEAK");
        REQUIRE(finding.severity == Severity::Warning);
        REQUIRE(finding.evidence.size() == 2);
        REQUIRE(finding.active);
    }
}
