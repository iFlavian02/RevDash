#pragma once

#include "revdash/core/bounded_spsc_queue.hpp"
#include "revdash/core/types.hpp"

namespace revdash::core {

constexpr std::size_t kSourceToEngineQueueCapacity = 1024;
constexpr std::size_t kEngineToRecorderQueueCapacity = 2048;

struct RecorderPacket {
    std::uint64_t engine_epoch{0};
    ObdMessage message{};
};

static_assert(std::is_trivially_copyable_v<ObdMessage>);
static_assert(std::is_trivially_copyable_v<RecorderPacket>);

using SourceToEngineQueue = BoundedSpscQueue<ObdMessage, kSourceToEngineQueueCapacity>;
using EngineToRecorderQueue = BoundedSpscQueue<RecorderPacket, kEngineToRecorderQueueCapacity>;

} // namespace revdash::core
