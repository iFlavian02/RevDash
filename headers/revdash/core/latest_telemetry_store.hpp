#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include "revdash/core/telemetry_types.hpp"

namespace revdash::core {

struct TelemetryStoreHealth {
    std::uint64_t read_lock_contentions{0};
    std::uint64_t write_lock_contentions{0};
};

class LatestTelemetryStore {
public:
    void update(const TelemetrySample& sample) noexcept {
        const auto index = static_cast<std::size_t>(sample.metric_id);
        if (index >= kMetricCount) {
            return;
        }

        std::unique_lock lock(mutex_, std::defer_lock);
        if (!lock.try_lock()) {
            write_lock_contentions_.fetch_add(1, std::memory_order_relaxed);
            lock.lock();
        }
        snapshot_.samples[index] = sample;
        snapshot_.snapshot_monotonic_ts = sample.monotonic_ts;
        snapshot_.snapshot_utc_ts = sample.utc_ts;
    }

    [[nodiscard]] TelemetrySnapshot snapshot() const noexcept {
        std::shared_lock lock(mutex_, std::defer_lock);
        if (!lock.try_lock()) {
            read_lock_contentions_.fetch_add(1, std::memory_order_relaxed);
            lock.lock();
        }
        return snapshot_;
    }

    void setEpoch(std::uint64_t epoch) noexcept {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (!lock.try_lock()) {
            write_lock_contentions_.fetch_add(1, std::memory_order_relaxed);
            lock.lock();
        }
        snapshot_.epoch = epoch;
    }

    [[nodiscard]] TelemetryStoreHealth health() const noexcept {
        return TelemetryStoreHealth{
            .read_lock_contentions = read_lock_contentions_.load(std::memory_order_relaxed),
            .write_lock_contentions = write_lock_contentions_.load(std::memory_order_relaxed)
        };
    }

private:
    mutable std::shared_mutex mutex_;
    TelemetrySnapshot snapshot_{};
    mutable std::atomic<std::uint64_t> read_lock_contentions_{0};
    std::atomic<std::uint64_t> write_lock_contentions_{0};
};

} // namespace revdash::core
