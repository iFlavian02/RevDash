#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "revdash/core/error.hpp"

namespace revdash::session {

enum class CsvPreset : std::uint8_t {
    RevDash,
    MegaLogViewer,
    TunerStudio
};

enum class UnitSystem : std::uint8_t {
    Metric,
    Imperial
};

struct CsvExportOptions {
    CsvPreset preset{CsvPreset::RevDash};
    UnitSystem units{UnitSystem::Metric};
    std::chrono::microseconds interval{std::chrono::milliseconds{100}};
};

[[nodiscard]] constexpr std::string_view toString(CsvPreset preset) noexcept {
    switch (preset) {
        case CsvPreset::RevDash: return "RevDash";
        case CsvPreset::MegaLogViewer: return "MegaLogViewer";
        case CsvPreset::TunerStudio: return "TunerStudio";
    }
    return "Unknown";
}

// Converts a completed RevDash Session v1 JSONL recording to a uniformly
// sampled CSV file. The destination is written beside a temporary file and is
// replaced only after the complete export has been flushed and closed.
[[nodiscard]] core::Result<void> exportSessionCsv(
    const std::filesystem::path& session_path,
    const std::filesystem::path& destination_path,
    const CsvExportOptions& options = {});

} // namespace revdash::session
