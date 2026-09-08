#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "revdash/core/async_data_source.hpp"
#include "revdash/session/session_recorder.hpp"

namespace revdash::drivers {

enum class PlaybackState : std::uint8_t { Stopped, Paused, Playing };

// Replays only canonical OBD messages into IDataSource subscribers. Recorded
// findings and Mode 04 audits remain historical artifacts and are deliberately
// exposed separately from findings re-evaluated by the current engine.
class PlaybackDataSource final : public core::AsyncDataSource {
public:
    explicit PlaybackDataSource(
        std::shared_ptr<core::IClock> clock = std::make_shared<core::SystemClockSource>());
    ~PlaybackDataSource() override;

    void play(core::CompletionCallback completion = {});
    void pause(core::CompletionCallback completion = {});
    void step(core::CompletionCallback completion = {});
    void stop(core::CompletionCallback completion = {});
    void seek(
        std::chrono::microseconds target,
        std::chrono::steady_clock::duration warmup,
        core::CompletionCallback completion = {});
    void setSpeedMultiplier(double multiplier, core::CompletionCallback completion = {});

    // Wakes deterministic/manual-clock playback without waiting for a wall-clock timer.
    void poll();

    [[nodiscard]] PlaybackState playbackState() const noexcept;
    [[nodiscard]] std::chrono::microseconds position() const noexcept;
    [[nodiscard]] std::chrono::microseconds duration() const noexcept;
    [[nodiscard]] bool indexWasRebuilt() const noexcept;
    [[nodiscard]] std::vector<session::HistoricalSessionRecord> historicalFindings() const;
    [[nodiscard]] std::vector<session::HistoricalSessionRecord> historicalMode04Audits() const;

protected:
    void startConnect(const core::DataSourceConfig& config, core::CompletionCallback completion) override;
    void startDisconnect(core::CompletionCallback completion) override;
    void startTransmit(const core::ObdRequest& request, core::CompletionCallback completion) override;

private:
    struct Impl;
    std::unique_ptr<Impl> playback_;

    void processDue(std::uint64_t generation);
    void scheduleWake(std::uint64_t generation);
};

} // namespace revdash::drivers
