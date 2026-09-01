#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "revdash/drivers/serial_transport.hpp"

using namespace revdash;

namespace {

class FakeSerialTransport final : public drivers::ISerialTransport {
public:
    [[nodiscard]] bool isOpen() const noexcept override { return open_; }
    [[nodiscard]] core::SerialConfig config() const override { return config_; }

    void open(core::SerialConfig config, drivers::SerialCompletionCallback completion) override {
        config_ = std::move(config);
        open_ = true;
        completion(core::makeSuccess());
    }

    void close(drivers::SerialCompletionCallback completion) override {
        open_ = false;
        completion(core::makeSuccess());
    }

    void read(std::span<std::uint8_t> buffer, drivers::SerialTransferCallback completion) override {
        if (!open_) {
            completion(core::makeError(core::ErrorCode::TransportNotConnected, "Fake transport is closed"));
            return;
        }
        const std::size_t count = std::min(buffer.size(), pending_read_.size());
        std::copy_n(pending_read_.begin(), count, buffer.begin());
        completion(count);
    }

    void write(std::span<const std::uint8_t> buffer, drivers::SerialTransferCallback completion) override {
        if (!open_) {
            completion(core::makeError(core::ErrorCode::TransportNotConnected, "Fake transport is closed"));
            return;
        }
        written_.assign(buffer.begin(), buffer.end());
        completion(buffer.size());
    }

    void cancel() override { cancelled_ = true; }

    void queueRead(std::vector<std::uint8_t> bytes) { pending_read_ = std::move(bytes); }
    [[nodiscard]] const std::vector<std::uint8_t>& written() const noexcept { return written_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    bool open_{false};
    bool cancelled_{false};
    core::SerialConfig config_{};
    std::vector<std::uint8_t> pending_read_;
    std::vector<std::uint8_t> written_;
};

} // namespace

TEST_CASE("Serial transport interface supports lifecycle and partial transfers", "[serial_transport]") {
    FakeSerialTransport transport;
    const core::SerialConfig config{.port_name = "COM12", .baud_rate = 115200};
    bool opened = false;
    transport.open(config, [&](core::Result<void> result) {
        REQUIRE(result.has_value());
        opened = true;
    });
    REQUIRE(opened);
    REQUIRE(transport.isOpen());
    REQUIRE(transport.config() == config);

    transport.queueRead({0x41, 0x0C});
    std::array<std::uint8_t, 8> read_buffer{};
    transport.read(read_buffer, [&](core::Result<std::size_t> result) {
        REQUIRE(result.has_value());
        REQUIRE(*result == 2);
    });
    REQUIRE(read_buffer[0] == 0x41);
    REQUIRE(read_buffer[1] == 0x0C);

    const std::array<std::uint8_t, 3> write_buffer{0x30, 0x31, 0x0D};
    transport.write(write_buffer, [&](core::Result<std::size_t> result) {
        REQUIRE(result.has_value());
        REQUIRE(*result == write_buffer.size());
    });
    REQUIRE(transport.written() == std::vector<std::uint8_t>(write_buffer.begin(), write_buffer.end()));

    transport.cancel();
    REQUIRE(transport.cancelled());
    transport.close([](core::Result<void> result) { REQUIRE(result.has_value()); });
    REQUIRE_FALSE(transport.isOpen());
}

TEST_CASE("Serial port configuration validation is deterministic", "[serial_transport]") {
    REQUIRE(drivers::isSupportedSerialBaudRate(9600));
    REQUIRE(drivers::isSupportedSerialBaudRate(38400));
    REQUIRE(drivers::isSupportedSerialBaudRate(115200));
    REQUIRE_FALSE(drivers::isSupportedSerialBaudRate(57600));

    const auto normalized = drivers::normalizeSerialPortName(" com12 ");
    REQUIRE(normalized.has_value());
    REQUIRE(*normalized == "COM12");
    REQUIRE_FALSE(drivers::normalizeSerialPortName("COM0").has_value());
    REQUIRE_FALSE(drivers::normalizeSerialPortName("COM000").has_value());
    REQUIRE_FALSE(drivers::normalizeSerialPortName("ttyUSB0").has_value());
    REQUIRE_FALSE(drivers::normalizeSerialPortName("COM1x").has_value());
}

TEST_CASE("Serial port enumeration produces normalized metadata", "[serial_transport]") {
    const auto ports = drivers::enumerateSerialPorts();
    for (const auto& port : ports) {
        const auto normalized = drivers::normalizeSerialPortName(port.port_name);
        REQUIRE(normalized.has_value());
        REQUIRE(*normalized == port.port_name);
        REQUIRE((port.vendor_id.empty() || port.vendor_id.size() == 4));
        REQUIRE((port.product_id.empty() || port.product_id.size() == 4));
    }
}
