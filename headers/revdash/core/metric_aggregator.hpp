#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <vector>

#include "revdash/core/telemetry_types.hpp"

namespace revdash::core {

struct MetricStatistics {
    double minimum{0.0};
    double maximum{0.0};
    double mean{0.0};
    double median{0.0};
    std::size_t count{0};
};

class MetricAggregator {
public:
    explicit MetricAggregator(MonotonicClock::duration window = std::chrono::seconds{30}) noexcept : window_(window) {}

    void ingest(const TelemetrySample& sample) {
        const auto index = static_cast<std::size_t>(sample.metric_id);
        if (index >= kMetricCount) { return; }
        last_[index] = sample;
        if (sample.quality != SampleQuality::Valid) { return; }
        auto& samples = values_[index];
        const auto position = std::upper_bound(samples.begin(), samples.end(), sample.monotonic_ts, [](const auto& time, const auto& entry) { return time < entry.monotonic_ts; });
        samples.insert(position, sample);
        prune(samples, sample.monotonic_ts);
    }

    [[nodiscard]] std::optional<MetricStatistics> statistics(MetricId metric, MonotonicTimePoint now) {
        const auto index = static_cast<std::size_t>(metric);
        if (index >= kMetricCount) { return std::nullopt; }
        auto& samples = values_[index];
        prune(samples, now);
        if (samples.empty()) { return std::nullopt; }
        std::vector<double> values;
        values.reserve(samples.size());
        double sum = 0.0;
        for (const auto& sample : samples) { values.push_back(sample.value); sum += sample.value; }
        std::sort(values.begin(), values.end());
        const auto middle = values.size() / 2U;
        const auto median = values.size() % 2U == 0U ? (values[middle - 1U] + values[middle]) / 2.0 : values[middle];
        return MetricStatistics{.minimum = values.front(), .maximum = values.back(), .mean = sum / static_cast<double>(values.size()), .median = median, .count = values.size()};
    }

    [[nodiscard]] SampleQuality quality(MetricId metric, MonotonicTimePoint now) const noexcept {
        const auto index = static_cast<std::size_t>(metric);
        if (index >= kMetricCount || !last_[index].has_value()) { return SampleQuality::Unsupported; }
        const auto& sample = *last_[index];
        if (sample.quality != SampleQuality::Valid) { return sample.quality; }
        return now - sample.monotonic_ts > staleAfter(metric) ? SampleQuality::Stale : SampleQuality::Valid;
    }

    void resetForSourceSwitch() noexcept { reset(); }
    void resetForPlaybackSeek() noexcept { reset(); }
    void setEpoch(std::uint64_t epoch) noexcept { if (epoch != epoch_) { epoch_ = epoch; reset(); } }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    [[nodiscard]] static constexpr MonotonicClock::duration staleAfter(MetricId metric) noexcept {
        switch (metric) {
            case MetricId::Rpm: case MetricId::VehicleSpeed: case MetricId::ThrottlePosition: case MetricId::Map: return std::chrono::seconds{1};
            case MetricId::FuelLevel: case MetricId::AmbientAirTemp: case MetricId::ModuleVoltage: return std::chrono::seconds{5};
            default: return std::chrono::seconds{2};
        }
    }

private:
    void prune(std::deque<TelemetrySample>& samples, MonotonicTimePoint now) const { while (!samples.empty() && now - samples.front().monotonic_ts > window_) { samples.pop_front(); } }
    void reset() noexcept { for (auto& samples : values_) { samples.clear(); } for (auto& sample : last_) { sample.reset(); } }
    MonotonicClock::duration window_;
    std::array<std::deque<TelemetrySample>, kMetricCount> values_{};
    std::array<std::optional<TelemetrySample>, kMetricCount> last_{};
    std::uint64_t epoch_{0};
};

} // namespace revdash::core
