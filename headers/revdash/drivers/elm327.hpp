#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "revdash/core/async_data_source.hpp"
#include "revdash/drivers/serial_transport.hpp"

namespace revdash::drivers {

enum class ElmResponseStatus { Success, NoData, Searching, BusInitError, UnableToConnect, Stopped, UnsupportedCommand, Malformed };

struct ElmResponse {
    ElmResponseStatus status{ElmResponseStatus::Success};
    std::vector<std::string> lines;
};

class ElmPromptParser {
public:
    [[nodiscard]] std::vector<ElmResponse> append(std::string_view bytes);
    void reset() noexcept;

private:
    [[nodiscard]] static ElmResponseStatus classify(const std::vector<std::string>& lines) noexcept;
    std::string current_line_;
    std::vector<std::string> lines_;
};

struct Elm327Stats {
    std::string adapter_identity;
    std::string protocol;
    std::chrono::milliseconds last_rtt{0};
    std::chrono::milliseconds ewma_rtt{0};
    std::uint32_t timeout_count{0};
    std::uint32_t malformed_response_count{0};
    std::uint32_t reconnect_count{0};
};

class Elm327DataSource final : public core::AsyncDataSource {
public:
    explicit Elm327DataSource(std::unique_ptr<ISerialTransport> transport = std::make_unique<AsioSerialTransport>());
    ~Elm327DataSource() override;

    [[nodiscard]] Elm327Stats stats() const;
    [[nodiscard]] std::vector<std::string> rawLines() const;

protected:
    void startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) override;
    void startDisconnect(core::CompletionCallback completion) override;
    void startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) override;

private:
    struct Command;
    void initialize(std::size_t index, core::CompletionCallback completion);
    void runCommand(std::string command, bool optional, std::function<void(core::Result<ElmResponse>)> completion);
    void readNext();
    void finishCommand(core::Result<ElmResponse> result);
    void normalize(const ElmResponse& response);
    [[nodiscard]] std::chrono::milliseconds commandTimeout() const noexcept;
    [[nodiscard]] static std::string requestText(const core::ObdRequest& request);

    std::unique_ptr<ISerialTransport> transport_;
    std::array<std::uint8_t, 256> read_buffer_{};
    std::unique_ptr<Command> active_command_;
    ElmPromptParser parser_;
    Elm327Stats stats_;
    std::vector<std::string> raw_lines_;
    std::uint64_t sequence_{0};
    std::uint64_t command_id_{0};
    std::chrono::milliseconds response_timeout_{2000};
    std::uint8_t initialization_attempt_{0};
};

} // namespace revdash::drivers
