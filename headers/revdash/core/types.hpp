#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace revdash::core {

constexpr std::string_view kApplicationName = "RevDash";
constexpr std::string_view kApplicationVersion = "0.1.0";

enum class BuildType {
    Debug,
    Release
};

} // namespace revdash::core
