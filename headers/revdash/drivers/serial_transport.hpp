#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "revdash/core/data_source.hpp"

namespace revdash::drivers {

struct SerialPortInfo {
    std::string port_name;
    std::string friendly_name;
    std::string vendor_id;
    std::string product_id;
    std::string device_description;
    bool is_bluetooth_spp{false};
};

using SerialCompletionCallback = std::function<void(core::Result<void>)>;
using SerialTransferCallback = std::function<void(core::Result<std::size_t>)>;

// Callers retain ownership of read/write buffers until their callback runs.
class ISerialTransport {
public:
    virtual ~ISerialTransport() = default;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual core::SerialConfig config() const = 0;

    virtual void open(core::SerialConfig config, SerialCompletionCallback completion) = 0;
    virtual void close(SerialCompletionCallback completion) = 0;
    virtual void read(std::span<std::uint8_t> buffer, SerialTransferCallback completion) = 0;
    virtual void write(std::span<const std::uint8_t> buffer, SerialTransferCallback completion) = 0;
    virtual void cancel() = 0;
};

class AsioSerialTransport final : public ISerialTransport {
public:
    AsioSerialTransport();
    ~AsioSerialTransport() override;

    AsioSerialTransport(const AsioSerialTransport&) = delete;
    AsioSerialTransport& operator=(const AsioSerialTransport&) = delete;

    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] core::SerialConfig config() const override;

    void open(core::SerialConfig config, SerialCompletionCallback completion) override;
    void close(SerialCompletionCallback completion) override;
    void read(std::span<std::uint8_t> buffer, SerialTransferCallback completion) override;
    void write(std::span<const std::uint8_t> buffer, SerialTransferCallback completion) override;
    void cancel() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool isSupportedSerialBaudRate(std::uint32_t baud_rate) noexcept;
[[nodiscard]] core::Result<std::string> normalizeSerialPortName(std::string port_name);
[[nodiscard]] std::vector<SerialPortInfo> enumerateSerialPorts();

} // namespace revdash::drivers
