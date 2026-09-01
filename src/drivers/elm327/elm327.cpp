#include "revdash/drivers/elm327.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace revdash::drivers {
namespace {
std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    return first < last ? std::string(first, last) : std::string{};
}
std::string upper(std::string value) { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); }); return value; }
bool isHex(std::string_view value) { return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }); }
core::Error malformed(std::string context) { return {core::ErrorDomain::Protocol, std::string{core::toString(core::ErrorCode::ProtocolMalformedResponse)}, "ELM327 returned a malformed response", false, std::move(context)}; }
}

std::vector<ElmResponse> ElmPromptParser::append(std::string_view bytes) {
    std::vector<ElmResponse> completed;
    for (const char character : bytes) {
        if (character == '>') {
            const auto line = trim(std::move(current_line_)); current_line_.clear();
            if (!line.empty()) lines_.push_back(std::move(line));
            completed.push_back({classify(lines_), std::move(lines_)}); lines_.clear();
        } else if (character == '\r' || character == '\n') {
            const auto line = trim(std::move(current_line_)); current_line_.clear();
            if (!line.empty()) lines_.push_back(std::move(line));
        } else { current_line_ += character; }
    }
    return completed;
}
void ElmPromptParser::reset() noexcept { current_line_.clear(); lines_.clear(); }
ElmResponseStatus ElmPromptParser::classify(const std::vector<std::string>& lines) noexcept {
    bool searching = false;
    for (const auto& line : lines) { const auto value = upper(line); if (value == "NO DATA") return ElmResponseStatus::NoData; if (value == "BUS INIT: ERROR") return ElmResponseStatus::BusInitError; if (value == "UNABLE TO CONNECT") return ElmResponseStatus::UnableToConnect; if (value == "STOPPED") return ElmResponseStatus::Stopped; if (value == "?") return ElmResponseStatus::UnsupportedCommand; if (value == "SEARCHING...") searching = true; }
    if (searching) return ElmResponseStatus::Searching;
    return ElmResponseStatus::Success;
}

struct Elm327DataSource::Command { std::uint64_t id; std::string text; bool optional; std::chrono::steady_clock::time_point started; std::function<void(core::Result<ElmResponse>)> completion; };
Elm327DataSource::Elm327DataSource(std::unique_ptr<ISerialTransport> transport) : AsyncDataSource(core::DataSourceType::SerialElm327), transport_(std::move(transport)) {}
Elm327DataSource::~Elm327DataSource() = default;
Elm327Stats Elm327DataSource::stats() const { return stats_; }
std::vector<std::string> Elm327DataSource::rawLines() const { return raw_lines_; }

void Elm327DataSource::startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) {
    const auto* serial = std::get_if<core::SerialConfig>(&config);
    if (!serial) { completion(core::makeError(core::ErrorCode::CoreInvalidState, "ELM327 source requires SerialConfig")); return; }
    response_timeout_ = serial->response_timeout;
    initialization_attempt_ = 0;
    transport_->open(*serial, [this, completion = std::move(completion)](core::Result<void> result) mutable { postToWorker([this, completion = std::move(completion), result = std::move(result)]() mutable { if (!result) { completion(tl::make_unexpected(result.error())); return; } initialize(0, std::move(completion)); }); });
}
void Elm327DataSource::initialize(std::size_t index, core::CompletionCallback completion) {
    static constexpr std::array<std::string_view, 8> commands{"ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATCAF1", "ATSP0", "ATI"};
    if (index == commands.size()) { completion(core::makeSuccess()); return; }
    const bool optional = commands[index] == "ATI";
    runCommand(std::string{commands[index]}, optional, [this, index, completion = std::move(completion), optional](core::Result<ElmResponse> result) mutable { if (!result && !optional) { if (initialization_attempt_++ < 2) { postAfterToWorker(std::chrono::milliseconds{100}, [this, completion = std::move(completion)]() mutable { initialize(0, std::move(completion)); }); return; } completion(tl::make_unexpected(result.error())); return; } if (result && commands[index] == "ATI" && !result->lines.empty()) stats_.adapter_identity = result->lines.front(); initialize(index + 1, std::move(completion)); });
}
void Elm327DataSource::startDisconnect(core::CompletionCallback completion) { active_command_.reset(); parser_.reset(); transport_->cancel(); transport_->close([this, completion = std::move(completion)](core::Result<void> result) mutable { postToWorker([completion = std::move(completion), result = std::move(result)]() mutable { completion(std::move(result)); }); }); }
void Elm327DataSource::startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) { runCommand(requestText(request), false, [this, completion = std::move(completion)](core::Result<ElmResponse> result) mutable { if (!result) { completion(tl::make_unexpected(result.error())); return; } normalize(*result); if (stats_.protocol.empty() && std::any_of(result->lines.begin(), result->lines.end(), [](const std::string& line) { const auto value = upper(line); return value != "SEARCHING..." && value != "NO DATA"; })) { runCommand("ATDP", true, [this, completion = std::move(completion)](core::Result<ElmResponse> protocol) mutable { if (protocol && !protocol->lines.empty()) stats_.protocol = protocol->lines.front(); completion(core::makeSuccess()); }); return; } completion(core::makeSuccess()); }); }
void Elm327DataSource::runCommand(std::string command, bool optional, std::function<void(core::Result<ElmResponse>)> completion) {
    if (active_command_) { completion(core::makeError(core::ErrorCode::CoreInvalidState, "ELM327 permits only one active command")); return; }
    parser_.reset(); const auto id = ++command_id_; active_command_ = std::make_unique<Command>(Command{id, command, optional, std::chrono::steady_clock::now(), std::move(completion)});
    postAfterToWorker(commandTimeout(), [this, id] { if (active_command_ && active_command_->id == id) { ++stats_.timeout_count; transport_->cancel(); finishCommand(core::makeError(core::ErrorCode::TransportTimeout, "ELM327 command timed out", true)); } });
    const std::string wire = command + "\r";
    transport_->write(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()), [this, wire](core::Result<std::size_t> result) { postToWorker([this, result = std::move(result)]() mutable { if (!result) { finishCommand(tl::make_unexpected(result.error())); return; } readNext(); }); });
}
void Elm327DataSource::readNext() { transport_->read(read_buffer_, [this](core::Result<std::size_t> result) { postToWorker([this, result = std::move(result)]() mutable { if (!result) { finishCommand(tl::make_unexpected(result.error())); return; } auto responses = parser_.append(std::string_view(reinterpret_cast<const char*>(read_buffer_.data()), *result)); if (!responses.empty()) { finishCommand(std::move(responses.front())); return; } if (active_command_) readNext(); }); }); }
void Elm327DataSource::finishCommand(core::Result<ElmResponse> result) { if (!active_command_) return; auto command = std::move(active_command_); if (result) { const auto command_text = upper(command->text); result->lines.erase(std::remove_if(result->lines.begin(), result->lines.end(), [&command_text](const std::string& line) { return upper(line) == command_text; }), result->lines.end()); const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - command->started); stats_.last_rtt = elapsed; stats_.ewma_rtt = stats_.ewma_rtt.count() == 0 ? elapsed : std::chrono::milliseconds((stats_.ewma_rtt.count() * 7 + elapsed.count()) / 8); for (const auto& line : result->lines) raw_lines_.push_back(line); if (result->status == ElmResponseStatus::UnsupportedCommand && !command->optional) result = tl::make_unexpected(malformed("Unsupported required AT command")); if (result->status == ElmResponseStatus::BusInitError || result->status == ElmResponseStatus::UnableToConnect || result->status == ElmResponseStatus::Stopped) result = core::makeError(core::ErrorCode::TransportNotConnected, "ELM327 could not communicate with the vehicle", true); } if (command->completion) command->completion(std::move(result)); }
std::chrono::milliseconds Elm327DataSource::commandTimeout() const noexcept { const auto adaptive = stats_.ewma_rtt.count() == 0 ? response_timeout_ : std::chrono::milliseconds{std::max<std::int64_t>(response_timeout_.count(), stats_.ewma_rtt.count() * 4)}; return std::clamp(adaptive, std::chrono::milliseconds{250}, std::chrono::milliseconds{10000}); }
std::string Elm327DataSource::requestText(const core::ObdRequest& request) { std::ostringstream out; out << std::hex << std::uppercase; out.width(2); out.fill('0'); out << static_cast<unsigned>(request.mode); if (request.mode == 0x01 || request.mode == 0x02 || request.mode == 0x09) { out.width(2); out << static_cast<unsigned>(request.pid); } for (const auto byte : request.extra_payload()) { out.width(2); out << static_cast<unsigned>(byte); } return out.str(); }
void Elm327DataSource::normalize(const ElmResponse& response) { for (const auto& original : response.lines) { const auto line = upper(original); if (line == "SEARCHING..." || line.rfind("AT", 0) == 0) continue; std::istringstream stream(line); std::vector<std::string> tokens; std::string token; while (stream >> token) tokens.push_back(token); if (tokens.empty()) continue; std::optional<core::EcuAddress> ecu; std::size_t start = 0; if ((tokens[0].size() == 3 || tokens[0].size() == 8) && isHex(tokens[0])) { const auto value = static_cast<std::uint32_t>(std::stoul(tokens[0], nullptr, 16)); ecu = core::EcuAddress{value, tokens[0].size() == 3 ? core::EcuAddressFormat::Can11Bit : core::EcuAddressFormat::Can29Bit}; start = 1; } std::string hex; for (; start < tokens.size(); ++start) hex += tokens[start]; if (hex.size() % 2 != 0 || !isHex(hex)) { ++stats_.malformed_response_count; continue; } std::vector<std::uint8_t> bytes; for (std::size_t i = 0; i < hex.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16))); if (bytes.size() > 1 && bytes.front() == bytes.size() - 1) bytes.erase(bytes.begin()); auto message = core::ObdMessage::create(core::DataSourceType::SerialElm327, ecu, bytes, ++sequence_); if (message) publishMessage(*message); else ++stats_.malformed_response_count; } }
} // namespace revdash::drivers
