#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "revdash/core/diagnostic_types.hpp"
#include "revdash/core/error.hpp"

namespace revdash::diagnostics {

inline constexpr int kDtcDatabaseSchemaVersion = 1;

enum class DtcDatabaseKind {
    Production,
    TestFixture
};

[[nodiscard]] constexpr std::string_view toString(DtcDatabaseKind kind) noexcept {
    switch (kind) {
        case DtcDatabaseKind::Production: return "production";
        case DtcDatabaseKind::TestFixture: return "fixture";
    }
    return "unknown";
}

struct DtcDefinition {
    std::string code;
    std::string description;
    core::Severity severity{core::Severity::Advisory};
    std::vector<std::string> likely_failure_points{};
    std::string source_version;
    bool known{false};
};

struct DtcDatabaseMetadata {
    int schema_version{0};
    std::string source_version;
    DtcDatabaseKind kind{DtcDatabaseKind::Production};
};

struct DtcImportOptions {
    std::filesystem::path input_csv;
    std::filesystem::path output_database;
    DtcDatabaseKind database_kind{DtcDatabaseKind::Production};
};

[[nodiscard]] core::Result<void> importDtcCsv(const DtcImportOptions& options);

class DtcDatabase final {
public:
    ~DtcDatabase();

    DtcDatabase(DtcDatabase&& other) noexcept;
    DtcDatabase& operator=(DtcDatabase&& other) noexcept;

    DtcDatabase(const DtcDatabase&) = delete;
    DtcDatabase& operator=(const DtcDatabase&) = delete;

    [[nodiscard]] static core::Result<DtcDatabase> openReadOnly(
        const std::filesystem::path& path,
        DtcDatabaseKind expected_kind = DtcDatabaseKind::Production
    );

    [[nodiscard]] const DtcDatabaseMetadata& metadata() const noexcept;
    [[nodiscard]] bool isReadOnly() const noexcept;

    [[nodiscard]] core::Result<DtcDefinition> lookupExact(std::string_view code) const;
    [[nodiscard]] core::Result<std::vector<DtcDefinition>> lookupPrefix(
        std::string_view prefix,
        std::size_t limit = 50
    ) const;
    [[nodiscard]] core::Result<std::vector<DtcDefinition>> searchKeywords(
        std::string_view keywords,
        std::size_t limit = 50
    ) const;

private:
    struct Impl;

    explicit DtcDatabase(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace revdash::diagnostics
