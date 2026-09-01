#include "revdash/diagnostics/dtc_database.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace revdash::diagnostics {
namespace {

constexpr int kDtcDatabaseApplicationId = 0x52445644; // "RDVD"
constexpr std::size_t kMaximumQueryResults = 500;
constexpr std::array<std::string_view, 5> kRequiredCsvFields{
    "code", "description", "severity", "likely_failure_points", "source_version"};

[[nodiscard]] tl::unexpected<core::Error> invalidDatabase(
    std::string message,
    std::string context = {}
) {
    return core::makeError(
        core::ErrorCode::DiagnosticsDatabaseInvalid,
        std::move(message),
        false,
        std::move(context)
    );
}

[[nodiscard]] tl::unexpected<core::Error> storageFailure(
    std::string message,
    std::string context = {}
) {
    return core::makeError(
        core::ErrorCode::StorageUnavailable,
        std::move(message),
        false,
        std::move(context)
    );
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

[[nodiscard]] std::string trim(std::string_view value) {
    auto first = value.begin();
    while (first != value.end() && std::isspace(static_cast<unsigned char>(*first)) != 0) {
        ++first;
    }
    auto last = value.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1))) != 0) {
        --last;
    }
    return {first, last};
}

[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7F) {
            ++index;
            continue;
        }
        if (first >= 0xC2 && first <= 0xDF) {
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

using CsvRows = std::vector<std::vector<std::string>>;

[[nodiscard]] core::Result<CsvRows> parseCsv(std::string content) {
    if (!isValidUtf8(content)) {
        return invalidDatabase("DTC CSV is not valid UTF-8");
    }
    if (content.starts_with("\xEF\xBB\xBF")) {
        content.erase(0, 3);
    }

    CsvRows rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool quote_closed = false;
    bool at_field_start = true;

    const auto finishField = [&] {
        row.push_back(std::move(field));
        field.clear();
        at_field_start = true;
        quote_closed = false;
    };
    const auto finishRow = [&] {
        finishField();
        const bool blank = row.size() == 1 && row.front().empty();
        if (!blank) {
            rows.push_back(std::move(row));
        }
        row.clear();
    };

    for (std::size_t index = 0; index < content.size(); ++index) {
        const char character = content[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < content.size() && content[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                    quote_closed = true;
                }
            } else {
                field.push_back(character);
            }
            continue;
        }

        if (quote_closed && character != ',' && character != '\r' && character != '\n') {
            return invalidDatabase("Unexpected character after a quoted CSV field");
        }
        if (character == '"') {
            if (!at_field_start) {
                return invalidDatabase("Unexpected quote in an unquoted CSV field");
            }
            quoted = true;
            at_field_start = false;
        } else if (character == ',') {
            finishField();
        } else if (character == '\r' || character == '\n') {
            if (character == '\r' && index + 1 < content.size() && content[index + 1] == '\n') {
                ++index;
            }
            finishRow();
        } else {
            field.push_back(character);
            at_field_start = false;
        }
    }
    if (quoted) {
        return invalidDatabase("DTC CSV ends inside a quoted field");
    }
    if (!field.empty() || !row.empty() || quote_closed) {
        finishRow();
    }
    if (rows.empty()) {
        return invalidDatabase("DTC CSV is empty");
    }
    return rows;
}

[[nodiscard]] std::string uppercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

[[nodiscard]] bool isValidDtcCode(std::string_view code) noexcept {
    if (code.size() != 5 || (code[0] != 'P' && code[0] != 'C' && code[0] != 'B' && code[0] != 'U') ||
        code[1] < '0' || code[1] > '3') {
        return false;
    }
    return std::all_of(code.begin() + 2, code.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

[[nodiscard]] bool isValidDtcPrefix(std::string_view prefix) noexcept {
    if (prefix.empty() || prefix.size() > 5 ||
        (prefix[0] != 'P' && prefix[0] != 'C' && prefix[0] != 'B' && prefix[0] != 'U')) {
        return false;
    }
    if (prefix.size() >= 2 && (prefix[1] < '0' || prefix[1] > '3')) {
        return false;
    }
    return prefix.size() <= 2 || std::all_of(prefix.begin() + 2, prefix.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

[[nodiscard]] std::optional<core::Severity> parseSeverity(std::string value) {
    value = uppercaseAscii(trim(value));
    if (value == "ADVISORY") return core::Severity::Advisory;
    if (value == "WARNING") return core::Severity::Warning;
    if (value == "CRITICAL") return core::Severity::Critical;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> splitFailurePoints(std::string_view value) {
    std::vector<std::string> points;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto delimiter = value.find('|', start);
        const auto part = trim(value.substr(start, delimiter == std::string_view::npos ? value.size() - start : delimiter - start));
        if (!part.empty()) {
            points.push_back(part);
        }
        if (delimiter == std::string_view::npos) {
            break;
        }
        start = delimiter + 1;
    }
    return points;
}

[[nodiscard]] std::vector<std::string> normalizedTokens(std::string_view value) {
    std::vector<std::string> tokens;
    std::string token;
    const auto finish = [&] {
        if (!token.empty()) {
            tokens.push_back(std::move(token));
            token.clear();
        }
    };
    for (const unsigned char character : value) {
        if (character >= 0x80U) {
            token.push_back(static_cast<char>(character));
        } else if (std::isalnum(character) != 0) {
            token.push_back(static_cast<char>(std::tolower(character)));
        } else {
            finish();
        }
    }
    finish();
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

struct ImportRow {
    std::string code;
    std::string description;
    core::Severity severity{core::Severity::Advisory};
    std::vector<std::string> likely_failure_points{};
    std::string source_version;
};

[[nodiscard]] core::Result<std::vector<ImportRow>> validateRows(const CsvRows& csv_rows) {
    const auto& header = csv_rows.front();
    if (header.size() != kRequiredCsvFields.size()) {
        return invalidDatabase("DTC CSV must contain exactly the five supported fields");
    }
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index) {
        const auto name = trim(header[index]);
        if (!columns.emplace(name, index).second) {
            return invalidDatabase("DTC CSV contains a duplicate column", name);
        }
    }
    for (const auto required : kRequiredCsvFields) {
        if (!columns.contains(std::string{required})) {
            return invalidDatabase("DTC CSV is missing a required field", std::string{required});
        }
    }

    std::vector<ImportRow> rows;
    std::unordered_set<std::string> codes;
    std::optional<std::string> dataset_version;
    for (std::size_t row_index = 1; row_index < csv_rows.size(); ++row_index) {
        const auto& source = csv_rows[row_index];
        const auto context = "row " + std::to_string(row_index + 1);
        if (source.size() != header.size()) {
            return invalidDatabase("DTC CSV row has the wrong number of fields", context);
        }
        auto code = uppercaseAscii(trim(source[columns.at("code")]));
        auto description = trim(source[columns.at("description")]);
        const auto severity = parseSeverity(source[columns.at("severity")]);
        auto failure_points = splitFailurePoints(source[columns.at("likely_failure_points")]);
        auto source_version = trim(source[columns.at("source_version")]);
        if (!isValidDtcCode(code)) {
            return invalidDatabase("DTC CSV contains an invalid diagnostic code", context);
        }
        if (description.empty() || failure_points.empty() || source_version.empty()) {
            return invalidDatabase("DTC CSV contains an empty required field", context);
        }
        if (!severity.has_value()) {
            return invalidDatabase("DTC CSV contains an unsupported severity", context);
        }
        if (!codes.insert(code).second) {
            return invalidDatabase("DTC CSV contains a duplicate or conflicting code", code);
        }
        if (!dataset_version.has_value()) {
            dataset_version = source_version;
        } else if (*dataset_version != source_version) {
            return invalidDatabase("DTC CSV contains conflicting source versions", context);
        }
        rows.push_back(ImportRow{
            .code = std::move(code),
            .description = std::move(description),
            .severity = *severity,
            .likely_failure_points = std::move(failure_points),
            .source_version = std::move(source_version)});
    }
    if (rows.empty()) {
        return invalidDatabase("DTC CSV contains no data rows");
    }
    return rows;
}

class Statement final {
public:
    Statement(sqlite3* database, std::string_view sql) {
        sqlite3_prepare_v2(database, std::string{sql}.c_str(), -1, &statement_, nullptr);
    }
    ~Statement() { finalize(); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }
    void finalize() noexcept {
        sqlite3_finalize(statement_);
        statement_ = nullptr;
    }

private:
    sqlite3_stmt* statement_{nullptr};
};

[[nodiscard]] core::Result<void> executeSql(sqlite3* database, std::string_view sql) {
    char* error_message = nullptr;
    const auto result = sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &error_message);
    if (result != SQLITE_OK) {
        std::string context = error_message == nullptr ? sqlite3_errmsg(database) : error_message;
        sqlite3_free(error_message);
        return storageFailure("SQLite operation failed", std::move(context));
    }
    return core::makeSuccess();
}

[[nodiscard]] bool bindText(sqlite3_stmt* statement, int index, std::string_view value) noexcept {
    return sqlite3_bind_text(
               statement,
               index,
               value.data(),
               static_cast<int>(value.size()),
               SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] core::Result<std::string> metadataValue(sqlite3* database, std::string_view key) {
    Statement statement{database, "SELECT value FROM metadata WHERE key = ?1"};
    if (!statement.valid() || !bindText(statement.get(), 1, key)) {
        return invalidDatabase("DTC database metadata cannot be read", std::string{key});
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return invalidDatabase("DTC database is missing required metadata", std::string{key});
    }
    const auto* value = sqlite3_column_text(statement.get(), 0);
    if (value == nullptr) {
        return invalidDatabase("DTC database contains invalid metadata", std::string{key});
    }
    return std::string{reinterpret_cast<const char*>(value)};
}

[[nodiscard]] core::Result<int> pragmaInteger(sqlite3* database, std::string_view pragma) {
    Statement statement{database, std::string{"PRAGMA "} + std::string{pragma}};
    if (!statement.valid() || sqlite3_step(statement.get()) != SQLITE_ROW) {
        return invalidDatabase("DTC database compatibility metadata cannot be read", std::string{pragma});
    }
    return sqlite3_column_int(statement.get(), 0);
}

[[nodiscard]] std::string prefixUpperBound(std::string prefix) {
    for (auto iterator = prefix.rbegin(); iterator != prefix.rend(); ++iterator) {
        if (static_cast<unsigned char>(*iterator) < std::numeric_limits<unsigned char>::max()) {
            *iterator = static_cast<char>(static_cast<unsigned char>(*iterator) + 1U);
            prefix.erase(iterator.base(), prefix.end());
            return prefix;
        }
    }
    return prefix + '\xFF';
}

[[nodiscard]] core::Result<DtcDefinition> readDefinition(sqlite3_stmt* statement) {
    const auto* code = sqlite3_column_text(statement, 0);
    const auto* description = sqlite3_column_text(statement, 1);
    const auto severity_value = sqlite3_column_int(statement, 2);
    const auto* points_json = sqlite3_column_text(statement, 3);
    const auto* source_version = sqlite3_column_text(statement, 4);
    if (code == nullptr || description == nullptr || points_json == nullptr || source_version == nullptr ||
        severity_value < static_cast<int>(core::Severity::Advisory) ||
        severity_value > static_cast<int>(core::Severity::Critical)) {
        return invalidDatabase("DTC database contains a malformed entry");
    }
    try {
        const auto points = nlohmann::json::parse(reinterpret_cast<const char*>(points_json));
        if (!points.is_array()) {
            return invalidDatabase("DTC database contains malformed failure-point data");
        }
        return DtcDefinition{
            .code = reinterpret_cast<const char*>(code),
            .description = reinterpret_cast<const char*>(description),
            .severity = static_cast<core::Severity>(severity_value),
            .likely_failure_points = points.get<std::vector<std::string>>(),
            .source_version = reinterpret_cast<const char*>(source_version),
            .known = true};
    } catch (const nlohmann::json::exception&) {
        return invalidDatabase("DTC database contains malformed failure-point data");
    }
}

[[nodiscard]] core::Result<std::vector<DtcDefinition>> collectDefinitions(sqlite3_stmt* statement) {
    std::vector<DtcDefinition> definitions;
    while (true) {
        const auto result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            return definitions;
        }
        if (result != SQLITE_ROW) {
            return invalidDatabase("DTC database query failed");
        }
        auto definition = readDefinition(statement);
        if (!definition) {
            return tl::make_unexpected(definition.error());
        }
        definitions.push_back(std::move(*definition));
    }
}

class TemporaryDatabase final {
public:
    explicit TemporaryDatabase(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryDatabase() {
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

} // namespace

struct DtcDatabase::Impl {
    sqlite3* database{nullptr};
    DtcDatabaseMetadata metadata{};

    ~Impl() { sqlite3_close(database); }
};

core::Result<void> importDtcCsv(const DtcImportOptions& options) {
    if (options.input_csv.empty() || options.output_database.empty()) {
        return storageFailure("DTC importer requires input and output paths");
    }
    std::ifstream input{options.input_csv, std::ios::binary};
    if (!input) {
        return storageFailure("DTC CSV cannot be opened", pathUtf8(options.input_csv));
    }
    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = parseCsv(contents);
    if (!parsed) {
        return tl::make_unexpected(parsed.error());
    }
    auto rows = validateRows(*parsed);
    if (!rows) {
        return tl::make_unexpected(rows.error());
    }
    if (std::filesystem::exists(options.output_database)) {
        return storageFailure("DTC output database already exists", pathUtf8(options.output_database));
    }
    const auto parent = options.output_database.has_parent_path()
                            ? options.output_database.parent_path()
                            : std::filesystem::current_path();
    if (!std::filesystem::is_directory(parent)) {
        return storageFailure("DTC output directory does not exist", pathUtf8(parent));
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary_path = options.output_database;
    temporary_path += ".tmp-" + std::to_string(nonce);
    TemporaryDatabase cleanup{temporary_path};

    sqlite3* database = nullptr;
    const auto open_result = sqlite3_open_v2(
        pathUtf8(temporary_path).c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE,
        nullptr);
    if (open_result != SQLITE_OK) {
        const std::string context = database == nullptr ? pathUtf8(temporary_path) : sqlite3_errmsg(database);
        sqlite3_close(database);
        return storageFailure("DTC output database cannot be created", context);
    }

    const auto closeWith = [&](const core::Error& error) -> core::Result<void> {
        sqlite3_close_v2(database);
        return tl::make_unexpected(error);
    };
    const std::string schema =
        "PRAGMA journal_mode=OFF;"
        "PRAGMA synchronous=FULL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA application_id=" + std::to_string(kDtcDatabaseApplicationId) + ";"
        "PRAGMA user_version=" + std::to_string(kDtcDatabaseSchemaVersion) + ";"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE dtcs("
        "code TEXT PRIMARY KEY COLLATE BINARY,"
        "description TEXT NOT NULL,"
        "severity INTEGER NOT NULL CHECK(severity BETWEEN 0 AND 2),"
        "likely_failure_points TEXT NOT NULL,"
        "source_version TEXT NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE INDEX idx_dtcs_code ON dtcs(code);"
        "CREATE TABLE dtc_search_terms("
        "term TEXT COLLATE BINARY NOT NULL,"
        "code TEXT COLLATE BINARY NOT NULL REFERENCES dtcs(code) ON DELETE CASCADE,"
        "PRIMARY KEY(term, code)"
        ") WITHOUT ROWID;"
        "CREATE INDEX idx_dtc_search_terms_code ON dtc_search_terms(code);"
        "BEGIN IMMEDIATE;";
    if (const auto result = executeSql(database, schema); !result) {
        return closeWith(result.error());
    }

    Statement metadata_insert{database, "INSERT INTO metadata(key, value) VALUES(?1, ?2)"};
    Statement dtc_insert{database, "INSERT INTO dtcs(code, description, severity, likely_failure_points, source_version) VALUES(?1, ?2, ?3, ?4, ?5)"};
    Statement term_insert{database, "INSERT OR IGNORE INTO dtc_search_terms(term, code) VALUES(?1, ?2)"};
    if (!metadata_insert.valid() || !dtc_insert.valid() || !term_insert.valid()) {
        const auto error = invalidDatabase("DTC database statements could not be prepared").value();
        static_cast<void>(executeSql(database, "ROLLBACK;"));
        return closeWith(error);
    }

    const std::array metadata_rows{
        std::pair<std::string_view, std::string>{"schema_version", std::to_string(kDtcDatabaseSchemaVersion)},
        std::pair<std::string_view, std::string>{"source_version", rows->front().source_version},
        std::pair<std::string_view, std::string>{"database_kind", std::string{toString(options.database_kind)}}};
    for (const auto& [key, value] : metadata_rows) {
        sqlite3_reset(metadata_insert.get());
        sqlite3_clear_bindings(metadata_insert.get());
        if (!bindText(metadata_insert.get(), 1, key) || !bindText(metadata_insert.get(), 2, value) ||
            sqlite3_step(metadata_insert.get()) != SQLITE_DONE) {
            const auto error = storageFailure("DTC database metadata could not be written", sqlite3_errmsg(database)).value();
            static_cast<void>(executeSql(database, "ROLLBACK;"));
            return closeWith(error);
        }
    }

    for (const auto& row : *rows) {
        sqlite3_reset(dtc_insert.get());
        sqlite3_clear_bindings(dtc_insert.get());
        const auto points_json = nlohmann::json(row.likely_failure_points).dump();
        const bool bound = bindText(dtc_insert.get(), 1, row.code) &&
                           bindText(dtc_insert.get(), 2, row.description) &&
                           sqlite3_bind_int(dtc_insert.get(), 3, static_cast<int>(row.severity)) == SQLITE_OK &&
                           bindText(dtc_insert.get(), 4, points_json) &&
                           bindText(dtc_insert.get(), 5, row.source_version);
        if (!bound || sqlite3_step(dtc_insert.get()) != SQLITE_DONE) {
            const auto error = storageFailure("DTC database row could not be written", row.code).value();
            static_cast<void>(executeSql(database, "ROLLBACK;"));
            return closeWith(error);
        }

        auto searchable = row.code + " " + row.description;
        for (const auto& point : row.likely_failure_points) {
            searchable += " " + point;
        }
        for (const auto& term : normalizedTokens(searchable)) {
            sqlite3_reset(term_insert.get());
            sqlite3_clear_bindings(term_insert.get());
            if (!bindText(term_insert.get(), 1, term) || !bindText(term_insert.get(), 2, row.code) ||
                sqlite3_step(term_insert.get()) != SQLITE_DONE) {
                const auto error = storageFailure("DTC search index could not be written", row.code).value();
                static_cast<void>(executeSql(database, "ROLLBACK;"));
                return closeWith(error);
            }
        }
    }

    if (const auto result = executeSql(database, "COMMIT; PRAGMA optimize;"); !result) {
        static_cast<void>(executeSql(database, "ROLLBACK;"));
        return closeWith(result.error());
    }
    metadata_insert.finalize();
    dtc_insert.finalize();
    term_insert.finalize();
    if (sqlite3_close(database) != SQLITE_OK) {
        return storageFailure("DTC output database could not be finalized", pathUtf8(temporary_path));
    }
    database = nullptr;

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, options.output_database, rename_error);
    if (rename_error) {
        return storageFailure("DTC output database could not be published", rename_error.message());
    }
    cleanup.release();
    return core::makeSuccess();
}

DtcDatabase::DtcDatabase(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DtcDatabase::~DtcDatabase() = default;
DtcDatabase::DtcDatabase(DtcDatabase&& other) noexcept = default;
DtcDatabase& DtcDatabase::operator=(DtcDatabase&& other) noexcept = default;

core::Result<DtcDatabase> DtcDatabase::openReadOnly(
    const std::filesystem::path& path,
    DtcDatabaseKind expected_kind
) {
    sqlite3* database = nullptr;
    const auto result = sqlite3_open_v2(pathUtf8(path).c_str(), &database, SQLITE_OPEN_READONLY, nullptr);
    if (result != SQLITE_OK) {
        const std::string context = database == nullptr ? pathUtf8(path) : sqlite3_errmsg(database);
        sqlite3_close(database);
        return storageFailure("DTC database cannot be opened read-only", context);
    }
    const auto fail = [&](const core::Error& error) -> core::Result<DtcDatabase> {
        sqlite3_close(database);
        return tl::make_unexpected(error);
    };
    if (sqlite3_db_readonly(database, "main") != 1) {
        return fail(invalidDatabase("DTC database did not open read-only").value());
    }
    if (const auto query_only = executeSql(database, "PRAGMA query_only=ON;"); !query_only) {
        return fail(query_only.error());
    }
    const auto application_id = pragmaInteger(database, "application_id");
    const auto schema_version = pragmaInteger(database, "user_version");
    if (!application_id || !schema_version || *application_id != kDtcDatabaseApplicationId ||
        *schema_version != kDtcDatabaseSchemaVersion) {
        return fail(invalidDatabase("DTC database has an unsupported schema or application identity").value());
    }
    const auto metadata_schema = metadataValue(database, "schema_version");
    const auto source_version = metadataValue(database, "source_version");
    const auto kind_value = metadataValue(database, "database_kind");
    if (!metadata_schema || !source_version || !kind_value ||
        *metadata_schema != std::to_string(kDtcDatabaseSchemaVersion)) {
        return fail(invalidDatabase("DTC database metadata is invalid or unsupported").value());
    }
    std::optional<DtcDatabaseKind> actual_kind;
    if (*kind_value == toString(DtcDatabaseKind::Production)) actual_kind = DtcDatabaseKind::Production;
    if (*kind_value == toString(DtcDatabaseKind::TestFixture)) actual_kind = DtcDatabaseKind::TestFixture;
    if (!actual_kind.has_value() || *actual_kind != expected_kind) {
        return fail(invalidDatabase(
            "DTC database identity does not match its intended runtime use",
            *kind_value).value());
    }

    auto impl = std::make_unique<Impl>();
    impl->database = database;
    impl->metadata = DtcDatabaseMetadata{
        .schema_version = *schema_version,
        .source_version = *source_version,
        .kind = *actual_kind};
    return DtcDatabase{std::move(impl)};
}

const DtcDatabaseMetadata& DtcDatabase::metadata() const noexcept { return impl_->metadata; }
bool DtcDatabase::isReadOnly() const noexcept { return sqlite3_db_readonly(impl_->database, "main") == 1; }

core::Result<DtcDefinition> DtcDatabase::lookupExact(std::string_view code) const {
    const auto normalized = uppercaseAscii(trim(code));
    if (!isValidDtcCode(normalized)) {
        return invalidDatabase("Exact DTC lookup requires a valid five-character code", normalized);
    }
    Statement statement{impl_->database,
                        "SELECT code, description, severity, likely_failure_points, source_version "
                        "FROM dtcs WHERE code = ?1"};
    if (!statement.valid() || !bindText(statement.get(), 1, normalized)) {
        return invalidDatabase("Exact DTC lookup could not be prepared");
    }
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return DtcDefinition{
            .code = normalized,
            .description = "Unknown diagnostic trouble code",
            .severity = core::Severity::Advisory,
            .likely_failure_points = {},
            .source_version = impl_->metadata.source_version,
            .known = false};
    }
    if (result != SQLITE_ROW) {
        return invalidDatabase("Exact DTC lookup failed");
    }
    return readDefinition(statement.get());
}

core::Result<std::vector<DtcDefinition>> DtcDatabase::lookupPrefix(
    std::string_view prefix,
    std::size_t limit
) const {
    const auto normalized = uppercaseAscii(trim(prefix));
    if (!isValidDtcPrefix(normalized)) {
        return invalidDatabase("DTC prefix lookup requires a valid code prefix", normalized);
    }
    Statement statement{impl_->database,
                        "SELECT code, description, severity, likely_failure_points, source_version "
                        "FROM dtcs WHERE code >= ?1 AND code < ?2 ORDER BY code LIMIT ?3"};
    const auto upper = prefixUpperBound(normalized);
    const auto bounded_limit = std::min(limit, kMaximumQueryResults);
    if (!statement.valid() || !bindText(statement.get(), 1, normalized) ||
        !bindText(statement.get(), 2, upper) ||
        sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(bounded_limit)) != SQLITE_OK) {
        return invalidDatabase("DTC prefix lookup could not be prepared");
    }
    return collectDefinitions(statement.get());
}

core::Result<std::vector<DtcDefinition>> DtcDatabase::searchKeywords(
    std::string_view keywords,
    std::size_t limit
) const {
    const auto tokens = normalizedTokens(keywords);
    if (tokens.empty()) {
        return invalidDatabase("DTC keyword lookup requires at least one searchable term");
    }
    std::string sql = "SELECT DISTINCT d.code, d.description, d.severity, d.likely_failure_points, d.source_version FROM dtcs d";
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        sql += " JOIN dtc_search_terms s" + std::to_string(index) + " ON s" + std::to_string(index) + ".code=d.code AND s" +
               std::to_string(index) + ".term>=?" + std::to_string(index * 2 + 1) + " AND s" + std::to_string(index) +
               ".term<?" + std::to_string(index * 2 + 2);
    }
    const auto limit_parameter = tokens.size() * 2 + 1;
    sql += " ORDER BY d.code LIMIT ?" + std::to_string(limit_parameter);
    Statement statement{impl_->database, sql};
    if (!statement.valid()) {
        return invalidDatabase("DTC keyword lookup could not be prepared");
    }
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto upper = prefixUpperBound(tokens[index]);
        if (!bindText(statement.get(), static_cast<int>(index * 2 + 1), tokens[index]) ||
            !bindText(statement.get(), static_cast<int>(index * 2 + 2), upper)) {
            return invalidDatabase("DTC keyword lookup terms could not be bound");
        }
    }
    const auto bounded_limit = std::min(limit, kMaximumQueryResults);
    if (sqlite3_bind_int64(
            statement.get(),
            static_cast<int>(limit_parameter),
            static_cast<sqlite3_int64>(bounded_limit)) != SQLITE_OK) {
        return invalidDatabase("DTC keyword lookup limit could not be bound");
    }
    return collectDefinitions(statement.get());
}

} // namespace revdash::diagnostics
