#include "revdash/protocol/isotp_trace.hpp"

#include <algorithm>
#include <string>

namespace revdash::protocol {
namespace {

[[nodiscard]] tl::unexpected<core::Error> malformed(std::string message) {
    return core::makeError(core::ErrorCode::ProtocolMalformedResponse, std::move(message));
}

[[nodiscard]] IsoTpFrame makeFrame(const IsoTpTraceFrame& trace, IsoTpFrameType type) {
    return IsoTpFrame{.type = type, .address = trace.address, .timestamp = trace.timestamp};
}

} // namespace

std::size_t IsoTpAddressHash::operator()(const IsoTpAddress& address) const noexcept {
    const auto source = core::EcuAddressHash{}(address.source);
    const auto destination = core::EcuAddressHash{}(address.destination);
    return source ^ (destination + 0x9e3779b9U + (source << 6U) + (source >> 2U));
}

core::Result<IsoTpFrame> parseIsoTpTraceFrame(const IsoTpTraceFrame& frame) {
    const auto bytes = frame.bytes();
    if (bytes.empty() || bytes.size() > frame.data.size()) {
        return malformed("ISO-TP trace frame has an invalid CAN data length");
    }
    const auto kind = static_cast<std::uint8_t>(bytes[0] >> 4U);
    const auto low = static_cast<std::uint8_t>(bytes[0] & 0x0FU);
    switch (kind) {
        case 0x0: {
            if (low == 0U || low > 7U || bytes.size() != static_cast<std::size_t>(low) + 1U) {
                return malformed("ISO-TP single frame has an invalid declared length");
            }
            auto result = makeFrame(frame, IsoTpFrameType::SingleFrame);
            result.declared_length = low;
            result.payload_length = low;
            std::copy_n(bytes.begin() + 1, low, result.payload.begin());
            return result;
        }
        case 0x1: {
            if (bytes.size() < 3U) {
                return malformed("ISO-TP first frame is truncated");
            }
            const auto declared = static_cast<std::uint16_t>((static_cast<std::uint16_t>(low) << 8U) | bytes[1]);
            if (declared <= 7U || declared > core::kMaxIsoTpPayloadBytes) {
                return malformed("ISO-TP first frame declares an invalid payload length");
            }
            auto result = makeFrame(frame, IsoTpFrameType::FirstFrame);
            result.declared_length = declared;
            result.payload_length = static_cast<std::uint8_t>(bytes.size() - 2U);
            std::copy(bytes.begin() + 2, bytes.end(), result.payload.begin());
            return result;
        }
        case 0x2: {
            if (bytes.size() < 2U) {
                return malformed("ISO-TP consecutive frame has no payload bytes");
            }
            auto result = makeFrame(frame, IsoTpFrameType::ConsecutiveFrame);
            result.sequence_number = low;
            result.payload_length = static_cast<std::uint8_t>(bytes.size() - 1U);
            std::copy(bytes.begin() + 1, bytes.end(), result.payload.begin());
            return result;
        }
        case 0x3: {
            if (bytes.size() < 3U || low > 2U) {
                return malformed("ISO-TP flow-control frame is malformed");
            }
            auto result = makeFrame(frame, IsoTpFrameType::FlowControl);
            result.flow_status = low;
            result.block_size = bytes[1];
            result.st_min = bytes[2];
            return result;
        }
        default:
            return malformed("ISO-TP trace frame has an unsupported PCI type");
    }
}

IsoTpTraceReassembler::IsoTpTraceReassembler(std::chrono::milliseconds timeout) noexcept : timeout_(timeout) {}

core::Result<std::optional<ReassembledIsoTpPayload>> IsoTpTraceReassembler::accept(const IsoTpFrame& frame) {
    if (frame.type == IsoTpFrameType::FlowControl) {
        return std::nullopt;
    }
    if (frame.type == IsoTpFrameType::SingleFrame) {
        return ReassembledIsoTpPayload{.address = frame.address, .completed_at = frame.timestamp, .payload = {frame.bytes().begin(), frame.bytes().end()}};
    }
    if (frame.type == IsoTpFrameType::FirstFrame) {
        PendingMessage pending{.declared_length = frame.declared_length, .expected_sequence = 1U, .last_timestamp = frame.timestamp, .payload = {frame.bytes().begin(), frame.bytes().end()}};
        if (pending.payload.size() >= pending.declared_length) {
            return malformed("ISO-TP first frame payload is inconsistent with its declared length");
        }
        pending.payload.reserve(pending.declared_length);
        pending_.insert_or_assign(frame.address, std::move(pending));
        return std::nullopt;
    }

    const auto found = pending_.find(frame.address);
    if (found == pending_.end()) {
        return malformed("ISO-TP consecutive frame has no matching first frame");
    }
    auto& pending = found->second;
    if (frame.timestamp - pending.last_timestamp > timeout_) {
        pending_.erase(found);
        return core::makeError(core::ErrorCode::TransportTimeout, "ISO-TP trace reassembly timed out");
    }
    if (frame.sequence_number != pending.expected_sequence) {
        pending_.erase(found);
        return malformed("ISO-TP consecutive frame sequence number is incorrect or duplicated");
    }
    const auto remaining = static_cast<std::size_t>(pending.declared_length) - pending.payload.size();
    const auto copy_count = std::min(remaining, frame.bytes().size());
    pending.payload.insert(pending.payload.end(), frame.bytes().begin(), frame.bytes().begin() + static_cast<std::ptrdiff_t>(copy_count));
    pending.last_timestamp = frame.timestamp;
    pending.expected_sequence = static_cast<std::uint8_t>((pending.expected_sequence + 1U) & 0x0FU);
    if (pending.payload.size() != pending.declared_length) {
        return std::nullopt;
    }
    auto completed = ReassembledIsoTpPayload{.address = frame.address, .completed_at = frame.timestamp, .payload = std::move(pending.payload)};
    pending_.erase(found);
    return completed;
}

std::size_t IsoTpTraceReassembler::expire(core::MonotonicTimePoint now) noexcept {
    const auto before = pending_.size();
    std::erase_if(pending_, [this, now](const auto& entry) { return now - entry.second.last_timestamp > timeout_; });
    return before - pending_.size();
}

void IsoTpTraceReassembler::reset() noexcept { pending_.clear(); }

core::Result<std::vector<IsoTpTraceFrame>> makeIsoTpTraceFrames(IsoTpAddress address, std::span<const std::uint8_t> payload, core::MonotonicTimePoint start, std::chrono::milliseconds spacing) {
    if (payload.empty() || payload.size() > core::kMaxIsoTpPayloadBytes) {
        return core::makeError(core::ErrorCode::ProtocolPayloadTooLarge, "ISO-TP fixture payload must contain 1 to 4095 bytes");
    }
    std::vector<IsoTpTraceFrame> frames;
    auto timestamp = start;
    if (payload.size() <= 7U) {
        IsoTpTraceFrame frame{.address = address, .timestamp = timestamp, .length = static_cast<std::uint8_t>(payload.size() + 1U)};
        frame.data[0] = static_cast<std::uint8_t>(payload.size());
        std::copy(payload.begin(), payload.end(), frame.data.begin() + 1);
        frames.push_back(frame);
        return frames;
    }
    IsoTpTraceFrame first{.address = address, .timestamp = timestamp, .length = 8};
    first.data[0] = static_cast<std::uint8_t>(0x10U | ((payload.size() >> 8U) & 0x0FU));
    first.data[1] = static_cast<std::uint8_t>(payload.size() & 0xFFU);
    std::copy_n(payload.begin(), 6, first.data.begin() + 2);
    frames.push_back(first);
    std::size_t offset = 6;
    std::uint8_t sequence = 1;
    while (offset < payload.size()) {
        timestamp += spacing;
        const auto count = std::min<std::size_t>(7, payload.size() - offset);
        IsoTpTraceFrame frame{.address = address, .timestamp = timestamp, .length = static_cast<std::uint8_t>(count + 1U)};
        frame.data[0] = static_cast<std::uint8_t>(0x20U | sequence);
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), count, frame.data.begin() + 1);
        frames.push_back(frame);
        offset += count;
        sequence = static_cast<std::uint8_t>((sequence + 1U) & 0x0FU);
    }
    return frames;
}

} // namespace revdash::protocol
