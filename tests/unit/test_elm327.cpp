#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "revdash/drivers/elm327.hpp"

using namespace revdash::drivers;

namespace {
class TranscriptTransport final : public ISerialTransport {
public:
    explicit TranscriptTransport(std::deque<std::string> transcript) : transcript_(std::move(transcript)) {}
    [[nodiscard]] bool isOpen() const noexcept override { return open_; }
    [[nodiscard]] revdash::core::SerialConfig config() const override { return config_; }
    void open(revdash::core::SerialConfig config, SerialCompletionCallback completion) override { config_ = std::move(config); open_ = true; completion(revdash::core::makeSuccess()); }
    void close(SerialCompletionCallback completion) override { open_ = false; completion(revdash::core::makeSuccess()); }
    void read(std::span<std::uint8_t> buffer, SerialTransferCallback completion) override { if (transcript_.empty()) return; const auto chunk = std::move(transcript_.front()); transcript_.pop_front(); std::copy(chunk.begin(), chunk.end(), buffer.begin()); completion(chunk.size()); }
    void write(std::span<const std::uint8_t> buffer, SerialTransferCallback completion) override { writes_.emplace_back(reinterpret_cast<const char*>(buffer.data()), buffer.size()); completion(buffer.size()); }
    void cancel() override { cancelled_ = true; }
    [[nodiscard]] const std::vector<std::string>& writes() const noexcept { return writes_; }
private:
    bool open_{false}; bool cancelled_{false}; revdash::core::SerialConfig config_{}; std::deque<std::string> transcript_; std::vector<std::string> writes_;
};

template <typename Predicate> bool waitFor(Predicate predicate) { const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1}; while (!predicate()) { if (std::chrono::steady_clock::now() >= until) return false; std::this_thread::yield(); } return true; }
} // namespace

TEST_CASE("ELM prompt parser accepts arbitrary chunks and prompt boundaries", "[elm327]") {
    ElmPromptParser parser;
    REQUIRE(parser.append("ATZ\rELM327 v1.5\r").empty());
    const auto responses = parser.append(">");
    REQUIRE(responses.size() == 1);
    REQUIRE(responses.front().status == ElmResponseStatus::Success);
    REQUIRE(responses.front().lines == std::vector<std::string>{"ATZ", "ELM327 v1.5"});
}

TEST_CASE("ELM prompt parser classifies adapter status lines", "[elm327]") {
    ElmPromptParser parser;
    const auto no_data = parser.append("SEARCHING...\rNO DATA\r>");
    REQUIRE(no_data.size() == 1);
    REQUIRE(no_data.front().status == ElmResponseStatus::NoData);
    const auto unavailable = parser.append("UNABLE TO CONNECT\r>");
    REQUIRE(unavailable.front().status == ElmResponseStatus::UnableToConnect);
    const auto unsupported = parser.append("?\r>");
    REQUIRE(unsupported.front().status == ElmResponseStatus::UnsupportedCommand);
}

TEST_CASE("ELM327 source initializes and preserves distinct CAN ECU responses", "[elm327]") {
    auto transport = std::make_unique<TranscriptTransport>(std::deque<std::string>{
        "ATZ\rELM327 v1.5\r>", "OK\r>", "OK\r>", "OK\r>", "OK\r>", "OK\r>", "OK\r>", "ELM327 v1.5\r>",
        "010C\r7E8 04 41 0C 1A F8\r7E9 04 41 0C 1B 00\r>"
    });
    auto* transport_ptr = transport.get();
    revdash::drivers::Elm327DataSource source(std::move(transport));
    std::atomic<bool> connected{false};
    std::vector<revdash::core::ObdMessage> messages;
    auto subscription = source.subscribe([&](const auto& message) { messages.push_back(message); }, nullptr);
    source.connect(revdash::core::SerialConfig{.port_name = "COM1"}, [&](auto result) { REQUIRE(result.has_value()); connected = true; });
    REQUIRE(waitFor([&] { return connected.load(); }));
    REQUIRE(transport_ptr->writes().size() == 8);
    std::atomic<bool> transmitted{false};
    source.transmit({.mode = 0x01, .pid = 0x0C}, [&](auto result) { REQUIRE(result.has_value()); transmitted = true; });
    REQUIRE(waitFor([&] { return transmitted.load(); }));
    REQUIRE(messages.size() == 2);
    REQUIRE(messages[0].ecu_address == revdash::core::EcuAddress{0x7E8});
    REQUIRE(messages[1].ecu_address == revdash::core::EcuAddress{0x7E9});
    REQUIRE(messages[0].payload()[0] == 0x41);
}
