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

} // namespace revdash::core
