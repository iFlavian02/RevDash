#include "revdash/drivers/pid_scheduler.hpp"

#include <algorithm>

namespace revdash::drivers {
void AdaptivePidScheduler::setSupportedPids(std::vector<std::uint8_t> pids) {
    std::sort(pids.begin(), pids.end()); pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    supported_ = std::move(pids); next_due_.assign(supported_.size(), core::MonotonicTimePoint{});
}
void AdaptivePidScheduler::enqueueDiagnostic(core::ObdRequest request) { diagnostics_.push_back(request); }
std::optional<core::ObdRequest> AdaptivePidScheduler::next(core::MonotonicTimePoint now) {
    if (in_flight_) return std::nullopt;
    if (!diagnostics_.empty()) { auto request = diagnostics_.front(); diagnostics_.pop_front(); in_flight_ = true; return request; }
    std::optional<std::size_t> selected;
    for (std::size_t i = 0; i < supported_.size(); ++i) {
        if (next_due_[i] > now) continue;
        if (!selected || tierFor(supported_[i]) < tierFor(supported_[*selected]) ||
            (tierFor(supported_[i]) == tierFor(supported_[*selected]) && next_due_[i] < next_due_[*selected])) selected = i;
    }
    if (!selected) return std::nullopt;
    const auto tier = tierFor(supported_[*selected]);
    next_due_[*selected] = now + periodFor(tier);
    in_flight_ = true;
    return core::ObdRequest{.mode = 0x01, .pid = supported_[*selected]};
}
void AdaptivePidScheduler::complete(core::MonotonicTimePoint, std::chrono::milliseconds round_trip, bool timed_out) {
    in_flight_ = false;
    const auto observed = timed_out ? std::max(round_trip, std::chrono::milliseconds{1000}) : round_trip;
    ewma_rtt_ = std::chrono::milliseconds{(ewma_rtt_.count() * 7 + observed.count()) / 8};
}
void AdaptivePidScheduler::setCongested(bool congested) noexcept { congested_ = congested; }
bool AdaptivePidScheduler::inFlight() const noexcept { return in_flight_; }
bool AdaptivePidScheduler::streamingPaused() const noexcept { return !diagnostics_.empty(); }
std::chrono::milliseconds AdaptivePidScheduler::dispatchInterval() const noexcept { return std::max(std::chrono::milliseconds{10}, std::chrono::milliseconds{ewma_rtt_.count() * 5 / 4}); }
PollTier AdaptivePidScheduler::tierFor(std::uint8_t pid) noexcept {
    if (pid == 0x0C || pid == 0x0D || pid == 0x11) return PollTier::High;
    if (pid == 0x0B || pid == 0x10 || pid == 0x04 || pid == 0x0E || (pid >= 0x14 && pid <= 0x2B)) return PollTier::Medium;
    return PollTier::Low;
}
std::chrono::milliseconds AdaptivePidScheduler::periodFor(PollTier tier) const noexcept {
    const auto base = dispatchInterval();
    if (tier == PollTier::High) return base * 3;
    if (tier == PollTier::Medium) return base * (congested_ ? 10 : 6);
    return base * (congested_ ? 20 : 12);
}
} // namespace revdash::drivers
