#include "revdash/drivers/serial_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>
#include <utility>

#include <boost/asio.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#endif

namespace revdash::drivers {
namespace {

core::Error transportError(const boost::system::error_code& error, std::string action, bool retryable = true) {
    if (error == boost::asio::error::operation_aborted) {
        return {
            .domain = core::ErrorDomain::Core,
            .code = std::string{core::toString(core::ErrorCode::CoreCancelled)},
            .message = "Serial operation cancelled",
            .retryable = false,
            .context = std::move(action)
        };
    }
    return {
        .domain = core::ErrorDomain::Transport,
        .code = std::string{core::toString(core::ErrorCode::TransportNotConnected)},
        .message = "Serial " + action + " failed: " + error.message(),
        .retryable = retryable,
        .context = error.message()
    };
}

core::Error invalidPortError(std::string port_name) {
    return {
        .domain = core::ErrorDomain::Transport,
        .code = std::string{core::toString(core::ErrorCode::TransportNotConnected)},
        .message = "Serial port name is invalid",
        .retryable = false,
        .context = std::move(port_name)
    };
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string{first, last} : std::string{};
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

bool containsCaseInsensitive(const std::string& value, const std::string& needle) {
    return uppercase(value).find(uppercase(needle)) != std::string::npos;
}

#if defined(_WIN32)
std::string registryString(HKEY key, const char* name) {
    DWORD type{};
    DWORD size{};
    if (RegQueryValueExA(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string deviceProperty(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type{};
    DWORD size{};
    SetupDiGetDeviceRegistryPropertyA(devices, &device, property, &type, nullptr, 0, &size);
    if (size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (!SetupDiGetDeviceRegistryPropertyA(
            devices, &device, property, &type, reinterpret_cast<PBYTE>(value.data()), size, nullptr)) {
        return {};
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string hardwareToken(const std::string& hardware_id, const std::string& key) {
    const auto marker = uppercase(hardware_id).find(key);
    if (marker == std::string::npos || marker + key.size() + 4 > hardware_id.size()) {
        return {};
    }
    return uppercase(hardware_id.substr(marker + key.size(), 4));
}
#endif

} // namespace

class AsioSerialTransport::Impl {
public:
    Impl()
        : work_guard(boost::asio::make_work_guard(io_context)),
          serial_port(io_context),
          worker([this](std::stop_token) { io_context.run(); }) {}

    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    boost::asio::serial_port serial_port;
    std::jthread worker;
    std::atomic<bool> open{false};
    mutable std::mutex config_mutex;
    core::SerialConfig config{};
};

AsioSerialTransport::AsioSerialTransport() : impl_(std::make_unique<Impl>()) {}

AsioSerialTransport::~AsioSerialTransport() {
    cancel();
    boost::asio::post(impl_->io_context, [impl = impl_.get()] {
        boost::system::error_code ignored;
        impl->serial_port.close(ignored);
        impl->open.store(false, std::memory_order_release);
    });
    impl_->work_guard.reset();
    impl_->worker.join();
}

bool AsioSerialTransport::isOpen() const noexcept {
    return impl_->open.load(std::memory_order_acquire);
}

core::SerialConfig AsioSerialTransport::config() const {
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    return impl_->config;
}

void AsioSerialTransport::open(core::SerialConfig config, SerialCompletionCallback completion) {
    boost::asio::post(impl_->io_context, [impl = impl_.get(), config = std::move(config), completion = std::move(completion)]() mutable {
        auto normalized = normalizeSerialPortName(std::move(config.port_name));
        if (!normalized.has_value()) {
            if (completion) {
                completion(tl::make_unexpected(normalized.error()));
            }
            return;
        }
        if (!isSupportedSerialBaudRate(config.baud_rate)) {
            if (completion) {
                completion(tl::make_unexpected(core::Error{
                    .domain = core::ErrorDomain::Transport,
                    .code = std::string{core::toString(core::ErrorCode::TransportNotConnected)},
                    .message = "Unsupported serial baud rate",
                    .retryable = false,
                    .context = std::to_string(config.baud_rate)
                }));
            }
            return;
        }

        config.port_name = std::move(*normalized);
        boost::system::error_code error;
        impl->serial_port.cancel(error);
        impl->serial_port.close(error);
        impl->serial_port.open(config.port_name, error);
        if (!error) {
            impl->serial_port.set_option(boost::asio::serial_port_base::baud_rate(config.baud_rate), error);
        }
        if (!error) {
            impl->serial_port.set_option(boost::asio::serial_port_base::character_size(8), error);
        }
        if (!error) {
            impl->serial_port.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none), error);
        }
        if (!error) {
            impl->serial_port.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one), error);
        }
        if (!error) {
            impl->serial_port.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none), error);
        }
        if (error) {
            const boost::system::error_code open_error = error;
            boost::system::error_code ignored;
            impl->serial_port.close(ignored);
            impl->open.store(false, std::memory_order_release);
            if (completion) {
                completion(tl::make_unexpected(transportError(open_error, "open")));
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(impl->config_mutex);
            impl->config = config;
        }
        impl->open.store(true, std::memory_order_release);
        if (completion) {
            completion(core::makeSuccess());
        }
    });
}

void AsioSerialTransport::close(SerialCompletionCallback completion) {
    boost::asio::post(impl_->io_context, [impl = impl_.get(), completion = std::move(completion)]() mutable {
        boost::system::error_code error;
        impl->serial_port.cancel(error);
        impl->serial_port.close(error);
        impl->open.store(false, std::memory_order_release);
        if (completion) {
            if (error) {
                completion(tl::make_unexpected(transportError(error, "close", false)));
            } else {
                completion(core::makeSuccess());
            }
        }
    });
}

void AsioSerialTransport::read(std::span<std::uint8_t> buffer, SerialTransferCallback completion) {
    boost::asio::post(impl_->io_context, [impl = impl_.get(), buffer, completion = std::move(completion)]() mutable {
        if (!impl->open.load(std::memory_order_acquire)) {
            if (completion) {
                completion(core::makeError(core::ErrorCode::TransportNotConnected, "Serial port is not open"));
            }
            return;
        }
        impl->serial_port.async_read_some(boost::asio::buffer(buffer.data(), buffer.size()),
            [completion = std::move(completion)](const boost::system::error_code& error, std::size_t size) mutable {
                if (!error) {
                    if (completion) {
                        completion(size);
                    }
                    return;
                }
                if (completion) {
                    completion(tl::make_unexpected(transportError(error, "read")));
                }
            });
    });
}

void AsioSerialTransport::write(std::span<const std::uint8_t> buffer, SerialTransferCallback completion) {
    boost::asio::post(impl_->io_context, [impl = impl_.get(), buffer, completion = std::move(completion)]() mutable {
        if (!impl->open.load(std::memory_order_acquire)) {
            if (completion) {
                completion(core::makeError(core::ErrorCode::TransportNotConnected, "Serial port is not open"));
            }
            return;
        }
        boost::asio::async_write(impl->serial_port, boost::asio::buffer(buffer.data(), buffer.size()),
            [completion = std::move(completion)](const boost::system::error_code& error, std::size_t size) mutable {
                if (!error) {
                    if (completion) {
                        completion(size);
                    }
                    return;
                }
                if (completion) {
                    completion(tl::make_unexpected(transportError(error, "write")));
                }
            });
    });
}

void AsioSerialTransport::cancel() {
    boost::asio::post(impl_->io_context, [impl = impl_.get()] {
        boost::system::error_code ignored;
        impl->serial_port.cancel(ignored);
    });
}

bool isSupportedSerialBaudRate(std::uint32_t baud_rate) noexcept {
    return baud_rate == 9600 || baud_rate == 38400 || baud_rate == 115200;
}

core::Result<std::string> normalizeSerialPortName(std::string port_name) {
    port_name = uppercase(trim(std::move(port_name)));
    if (port_name.size() < 4 || port_name.rfind("COM", 0) != 0) {
        return tl::make_unexpected(invalidPortError(std::move(port_name)));
    }
    const std::string suffix = port_name.substr(3);
    if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        }) || std::all_of(suffix.begin(), suffix.end(), [](char character) {
            return character == '0';
        })) {
        return tl::make_unexpected(invalidPortError(std::move(port_name)));
    }
    return port_name;
}

std::vector<SerialPortInfo> enumerateSerialPorts() {
    std::vector<SerialPortInfo> ports;
#if defined(_WIN32)
    HDEVINFO devices = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        return ports;
    }
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            break;
        }
        HKEY device_key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (device_key == INVALID_HANDLE_VALUE) {
            continue;
        }
        const std::string port_name = uppercase(trim(registryString(device_key, "PortName")));
        RegCloseKey(device_key);
        if (!normalizeSerialPortName(port_name).has_value()) {
            continue;
        }
        const std::string hardware_id = deviceProperty(devices, device, SPDRP_HARDWAREID);
        const std::string friendly_name = deviceProperty(devices, device, SPDRP_FRIENDLYNAME);
        const std::string description = deviceProperty(devices, device, SPDRP_DEVICEDESC);
        ports.push_back({
            .port_name = port_name,
            .friendly_name = friendly_name.empty() ? description : friendly_name,
            .vendor_id = hardwareToken(hardware_id, "VID_"),
            .product_id = hardwareToken(hardware_id, "PID_"),
            .device_description = description,
            .is_bluetooth_spp = containsCaseInsensitive(hardware_id, "BTHENUM") ||
                                containsCaseInsensitive(friendly_name, "BLUETOOTH") ||
                                containsCaseInsensitive(description, "BLUETOOTH")
        });
    }
    SetupDiDestroyDeviceInfoList(devices);
#endif
    return ports;
}

} // namespace revdash::drivers
