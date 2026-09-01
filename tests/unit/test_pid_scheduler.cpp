#include <catch2/catch_test_macros.hpp>
#include "revdash/drivers/pid_scheduler.hpp"

using namespace revdash;

TEST_CASE("Adaptive PID scheduler prioritizes supported live requests and enforces single flight", "[pid_scheduler]") {
    drivers::AdaptivePidScheduler scheduler;
    scheduler.setSupportedPids({0x05, 0x0C, 0x0D, 0x11, 0x42});
    const auto now = core::MonotonicTimePoint{};
    const auto first = scheduler.next(now);
    REQUIRE(first.has_value()); REQUIRE(first->pid == 0x0C);
    REQUIRE_FALSE(scheduler.next(now).has_value());
    scheduler.complete(now, std::chrono::milliseconds{40});
    const auto second = scheduler.next(now);
    REQUIRE(second.has_value()); REQUIRE(second->pid == 0x0D);
}

TEST_CASE("Adaptive PID scheduler drains diagnostic work before streaming and adapts to congestion", "[pid_scheduler]") {
    drivers::AdaptivePidScheduler scheduler;
    scheduler.setSupportedPids({0x0C, 0x05});
    scheduler.enqueueDiagnostic({.mode = 0x03});
    const auto now = core::MonotonicTimePoint{};
    REQUIRE(scheduler.streamingPaused());
    const auto diagnostic = scheduler.next(now);
    REQUIRE(diagnostic.has_value()); REQUIRE(diagnostic->mode == 0x03);
    scheduler.complete(now, std::chrono::milliseconds{500}, true);
    scheduler.setCongested(true);
    const auto interval = scheduler.dispatchInterval();
    REQUIRE(interval >= std::chrono::milliseconds{10});
    const auto live = scheduler.next(now);
    REQUIRE(live.has_value()); REQUIRE(live->pid == 0x0C);
}

TEST_CASE("Adaptive PID scheduler is fair within a tier and never invents unsupported PIDs", "[pid_scheduler]") {
    drivers::AdaptivePidScheduler scheduler;
    scheduler.setSupportedPids({0x0C, 0x0D, 0x11});
    const auto now = core::MonotonicTimePoint{};
    const auto first = scheduler.next(now); REQUIRE(first->pid == 0x0C);
    scheduler.complete(now, std::chrono::milliseconds{20});
    const auto second = scheduler.next(now); REQUIRE(second->pid == 0x0D);
    scheduler.complete(now, std::chrono::milliseconds{20});
    const auto third = scheduler.next(now); REQUIRE(third->pid == 0x11);
    REQUIRE(first->pid != 0x05); REQUIRE(second->pid != 0x05); REQUIRE(third->pid != 0x05);
}

TEST_CASE("Adaptive PID scheduler backs off low tiers first and recovers after congestion", "[pid_scheduler]") {
    drivers::AdaptivePidScheduler scheduler;
    scheduler.setSupportedPids({0x0C, 0x05});
    const auto now = core::MonotonicTimePoint{};
    const auto normal = scheduler.next(now); REQUIRE(normal->pid == 0x0C);
    scheduler.complete(now, std::chrono::milliseconds{40});
    scheduler.setCongested(true);
    const auto congested_interval = scheduler.dispatchInterval();
    scheduler.complete(now, std::chrono::milliseconds{20});
    scheduler.setCongested(false);
    REQUIRE(scheduler.dispatchInterval() <= congested_interval);
}

TEST_CASE("Adaptive PID scheduler prevents diagnostic starvation after streaming completion", "[pid_scheduler]") {
    drivers::AdaptivePidScheduler scheduler;
    scheduler.setSupportedPids({0x0C});
    const auto now = core::MonotonicTimePoint{};
    REQUIRE(scheduler.next(now)->pid == 0x0C);
    scheduler.enqueueDiagnostic({.mode = 0x07});
    REQUIRE_FALSE(scheduler.next(now).has_value());
    scheduler.complete(now, std::chrono::milliseconds{100}, true);
    const auto diagnostic = scheduler.next(now);
    REQUIRE(diagnostic.has_value()); REQUIRE(diagnostic->mode == 0x07);
}
