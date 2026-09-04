#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "revdash/core/types.hpp"

namespace revdash::drivers {

enum class PollTier : std::uint8_t { High, Medium, Low };

class AdaptivePidScheduler {
public:
    void setSupportedPids(std::vector<std::uint8_t> pids);
    void enqueueDiagnostic(core::ObdRequest request);
    [[nodiscard]] std::optional<core::ObdRequest> next(core::MonotonicTimePoint now);
    void complete(core::MonotonicTimePoint now, std::chrono::milliseconds round_trip, bool timed_out = false);
    void setCongested(bool congested) noexcept;
    void reset();
    [[nodiscard]] bool inFlight() const noexcept;
    [[nodiscard]] bool streamingPaused() const noexcept;
    [[nodiscard]] std::chrono::milliseconds dispatchInterval() const noexcept;

private:
    [[nodiscard]] static PollTier tierFor(std::uint8_t pid) noexcept;
    [[nodiscard]] std::chrono::milliseconds periodFor(PollTier tier) const noexcept;
    std::vector<std::uint8_t> supported_;
    std::vector<core::MonotonicTimePoint> next_due_;
    std::deque<core::ObdRequest> diagnostics_;
    bool in_flight_{false};
    bool congested_{false};
    std::chrono::milliseconds ewma_rtt_{50};
};

} // namespace revdash::drivers
