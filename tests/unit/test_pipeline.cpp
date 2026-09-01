#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>
#include "revdash/core/bounded_spsc_queue.hpp"
#include "revdash/core/latest_telemetry_store.hpp"
#include "revdash/core/pipeline_packets.hpp"

using namespace revdash::core;

namespace {

std::atomic<bool> g_track_allocations{false};
std::atomic<std::size_t> g_allocation_count{0};

void countAllocation() noexcept {
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

void* operator new(std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

TEST_CASE("BoundedSpscQueue preserves FIFO ordering and rejects overflow", "[spsc]") {
    BoundedSpscQueue<std::uint32_t, 4> queue;

    REQUIRE(queue.tryPush(10));
    REQUIRE(queue.tryPush(20));
    REQUIRE(queue.tryPush(30));
    REQUIRE(queue.tryPush(40));
    REQUIRE_FALSE(queue.tryPush(50));

    std::uint32_t value{};
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 10);
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 20);
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 30);
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 40);
    REQUIRE_FALSE(queue.tryPop(value));

    const auto health = queue.health();
    REQUIRE(health.pushed == 4);
    REQUIRE(health.popped == 4);
    REQUIRE(health.dropped == 1);
    REQUIRE(BoundedSpscQueue<std::uint32_t, 4>::kDropPolicy == QueueDropPolicy::RejectNewest);
}

TEST_CASE("BoundedSpscQueue remains lossless under single-producer single-consumer load", "[spsc]") {
    constexpr std::uint32_t message_count = 100'000;
    BoundedSpscQueue<std::uint32_t, 256> queue;
    std::atomic<bool> producer_done{false};
    std::atomic<bool> corrupted{false};

    std::jthread producer([&] {
        for (std::uint32_t value = 0; value < message_count; ++value) {
            while (!queue.tryPush(value)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::jthread consumer([&] {
        std::uint32_t expected = 0;
        std::uint32_t value{};
        while (expected < message_count) {
            if (!queue.tryPop(value)) {
                if (producer_done.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                continue;
            }
            if (value != expected++) {
                corrupted.store(true, std::memory_order_release);
                return;
            }
        }
    });
    producer.join();
    consumer.join();

    const auto health = queue.health();
    REQUIRE_FALSE(corrupted.load(std::memory_order_acquire));
    REQUIRE(health.pushed == message_count);
    REQUIRE(health.popped == message_count);
}

TEST_CASE("Pipeline queue aliases provide the documented fixed capacities", "[spsc]") {
    REQUIRE(SourceToEngineQueue::kCapacity == 1024);
    REQUIRE(EngineToRecorderQueue::kCapacity == 2048);
    REQUIRE(std::is_trivially_copyable_v<RecorderPacket>);
}

TEST_CASE("BoundedSpscQueue performs no steady-state heap allocation", "[spsc]") {
    BoundedSpscQueue<std::uint32_t, 8> queue;
    std::uint32_t value{};

    REQUIRE(queue.tryPush(1));
    REQUIRE(queue.tryPop(value));

    g_allocation_count.store(0, std::memory_order_relaxed);
    g_track_allocations.store(true, std::memory_order_relaxed);
    for (std::uint32_t iteration = 0; iteration < 10'000; ++iteration) {
        REQUIRE(queue.tryPush(iteration));
        REQUIRE(queue.tryPop(value));
        REQUIRE(value == iteration);
    }
    g_track_allocations.store(false, std::memory_order_relaxed);

    REQUIRE(g_allocation_count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("LatestTelemetryStore snapshots coherent metric tuples", "[latest_store]") {
    LatestTelemetryStore store;
    constexpr std::uint64_t update_count = 50'000;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> inconsistent_snapshot{false};

    std::jthread writer([&] {
        for (std::uint64_t sequence = 1; sequence <= update_count; ++sequence) {
            store.update(TelemetrySample{
                .metric_id = MetricId::Rpm,
                .value = static_cast<double>(sequence),
                .quality = SampleQuality::Valid,
                .monotonic_ts = MonotonicTimePoint{std::chrono::microseconds{sequence}},
                .utc_ts = UtcTimePoint{std::chrono::microseconds{sequence}},
                .sequence_number = sequence,
                .ecu_address = EcuAddress{0x7E8}
            });
        }
        writer_done.store(true, std::memory_order_release);
    });
    std::jthread reader([&] {
        while (!writer_done.load(std::memory_order_acquire)) {
            const auto snapshot = store.snapshot();
            const auto& sample = snapshot.get(MetricId::Rpm);
            if (sample.sequence_number == 0) {
                continue;
            }
            const auto expected_time = MonotonicTimePoint{std::chrono::microseconds{sample.sequence_number}};
            if (sample.value != static_cast<double>(sample.sequence_number) ||
                sample.monotonic_ts != expected_time ||
                sample.quality != SampleQuality::Valid ||
                sample.ecu_address != EcuAddress{0x7E8}) {
                inconsistent_snapshot.store(true, std::memory_order_release);
                return;
            }
        }
    });
    writer.join();
    reader.join();

    const auto snapshot = store.snapshot();
    REQUIRE_FALSE(inconsistent_snapshot.load(std::memory_order_acquire));
    REQUIRE(snapshot.get(MetricId::Rpm).sequence_number == update_count);
    REQUIRE(snapshot.get(MetricId::Rpm).value == static_cast<double>(update_count));
}

TEST_CASE("LatestTelemetryStore publishes complete snapshots and lock health", "[latest_store]") {
    LatestTelemetryStore store;
    const auto timestamp = MonotonicTimePoint{std::chrono::seconds{5}};
    store.setEpoch(3);
    store.update(TelemetrySample{
        .metric_id = MetricId::CoolantTemp,
        .value = 91.5,
        .quality = SampleQuality::Valid,
        .monotonic_ts = timestamp,
        .sequence_number = 9
    });

    const auto snapshot = store.snapshot();
    REQUIRE(snapshot.epoch == 3);
    REQUIRE(snapshot.snapshot_monotonic_ts == timestamp);
    REQUIRE(snapshot.get(MetricId::CoolantTemp).value == 91.5);
    REQUIRE(snapshot.get(MetricId::CoolantTemp).sequence_number == 9);

    const auto health = store.health();
    REQUIRE(health.read_lock_contentions == 0);
    REQUIRE(health.write_lock_contentions == 0);
}
