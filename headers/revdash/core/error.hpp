#pragma once

#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace revdash::core {

enum class ErrorDomain {
    Core,
    Transport,
    Protocol,
    Diagnostics,
    Session,
    Storage
};

enum class ErrorCode {
    CoreCancelled,
    CoreInvalidState,
    CoreUnsupportedPlatform,
    TransportNotConnected,
    TransportTimeout,
    ProtocolPayloadTooLarge,
    ProtocolMalformedResponse,
    ProtocolNegativeResponse,
    DiagnosticsUnsupported,
    SessionInvalidFormat,
    StorageUnavailable
};

[[nodiscard]] constexpr std::string_view toString(ErrorDomain domain) noexcept {
    switch (domain) {
        case ErrorDomain::Core: return "Core";
        case ErrorDomain::Transport: return "Transport";
        case ErrorDomain::Protocol: return "Protocol";
        case ErrorDomain::Diagnostics: return "Diagnostics";
        case ErrorDomain::Session: return "Session";
        case ErrorDomain::Storage: return "Storage";
    }
    return "Unknown";
}

[[nodiscard]] constexpr ErrorDomain errorDomain(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::CoreCancelled:
        case ErrorCode::CoreInvalidState:
        case ErrorCode::CoreUnsupportedPlatform: return ErrorDomain::Core;
        case ErrorCode::TransportNotConnected:
        case ErrorCode::TransportTimeout: return ErrorDomain::Transport;
        case ErrorCode::ProtocolPayloadTooLarge:
        case ErrorCode::ProtocolMalformedResponse:
        case ErrorCode::ProtocolNegativeResponse: return ErrorDomain::Protocol;
        case ErrorCode::DiagnosticsUnsupported: return ErrorDomain::Diagnostics;
        case ErrorCode::SessionInvalidFormat: return ErrorDomain::Session;
        case ErrorCode::StorageUnavailable: return ErrorDomain::Storage;
    }
    return ErrorDomain::Core;
}

[[nodiscard]] constexpr std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::CoreCancelled: return "Core.Cancelled";
        case ErrorCode::CoreInvalidState: return "Core.InvalidState";
        case ErrorCode::CoreUnsupportedPlatform: return "Core.UnsupportedPlatform";
        case ErrorCode::TransportNotConnected: return "Transport.NotConnected";
        case ErrorCode::TransportTimeout: return "Transport.Timeout";
        case ErrorCode::ProtocolPayloadTooLarge: return "Protocol.PayloadTooLarge";
        case ErrorCode::ProtocolMalformedResponse: return "Protocol.MalformedResponse";
        case ErrorCode::ProtocolNegativeResponse: return "Protocol.NegativeResponse";
        case ErrorCode::DiagnosticsUnsupported: return "Diagnostics.Unsupported";
        case ErrorCode::SessionInvalidFormat: return "Session.InvalidFormat";
        case ErrorCode::StorageUnavailable: return "Storage.Unavailable";
    }
    return "Core.Unknown";
}

struct Error {
    ErrorDomain domain{ErrorDomain::Core};
    std::string code;
    std::string message;
    bool retryable{false};
    std::string context;

    [[nodiscard]] bool operator==(const Error& other) const noexcept {
        return domain == other.domain && code == other.code &&
               message == other.message && retryable == other.retryable &&
               context == other.context;
    }
};

template <typename T>
using Result = tl::expected<T, Error>;

inline Result<void> makeSuccess() {
    return {};
}

inline tl::unexpected<Error> makeError(
    ErrorDomain domain,
    std::string code,
    std::string message,
    bool retryable = false,
    std::string context = {}
) {
    return tl::make_unexpected(Error{
        .domain = domain,
        .code = std::move(code),
        .message = std::move(message),
        .retryable = retryable,
        .context = std::move(context)
    });
}

inline tl::unexpected<Error> makeError(
    ErrorCode code,
    std::string message,
    bool retryable = false,
    std::string context = {}
) {
    return makeError(errorDomain(code), std::string{toString(code)}, std::move(message), retryable, std::move(context));
}

} // namespace revdash::core
