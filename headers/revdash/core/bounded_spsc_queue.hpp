#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <boost/lockfree/spsc_queue.hpp>

namespace revdash::core {

enum class QueueDropPolicy : std::uint8_t {
    RejectNewest
};

struct QueueHealth {
    std::uint64_t pushed{0};
    std::uint64_t popped{0};
    std::uint64_t dropped{0};
};

template <typename T, std::size_t Capacity>
class BoundedSpscQueue {
    static_assert(Capacity > 0, "SPSC queue capacity must be non-zero");
    static_assert(std::is_trivially_copyable_v<T>, "SPSC queue payloads must not require heap-managed copy operations");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr QueueDropPolicy kDropPolicy = QueueDropPolicy::RejectNewest;

    [[nodiscard]] bool tryPush(const T& value) noexcept {
        if (!queue_.push(value)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        pushed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool tryPop(T& value) noexcept {
        if (!queue_.pop(value)) {
            return false;
        }
        popped_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] QueueHealth health() const noexcept {
        return QueueHealth{
            .pushed = pushed_.load(std::memory_order_relaxed),
            .popped = popped_.load(std::memory_order_relaxed),
            .dropped = dropped_.load(std::memory_order_relaxed)
        };
    }

private:
    boost::lockfree::spsc_queue<T, boost::lockfree::capacity<Capacity>> queue_;
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> popped_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace revdash::core
