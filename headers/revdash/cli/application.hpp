#pragma once

#include <atomic>
#include <iosfwd>
#include <string>
#include <vector>

namespace revdash::cli {

enum class ExitCode : int {
    Success = 0,
    Usage = 2,
    Connection = 3,
    Protocol = 4,
    SafetyRejection = 5,
    Io = 6
};

// An injectable interruption point keeps Ctrl+C behavior deterministic in tests.
class InterruptFlag {
public:
    virtual ~InterruptFlag() = default;
    [[nodiscard]] virtual bool requested() const noexcept = 0;
};

class ConsoleInterruptFlag final : public InterruptFlag {
public:
    ConsoleInterruptFlag();
    ~ConsoleInterruptFlag() override;

    ConsoleInterruptFlag(const ConsoleInterruptFlag&) = delete;
    ConsoleInterruptFlag& operator=(const ConsoleInterruptFlag&) = delete;

    [[nodiscard]] bool requested() const noexcept override;

private:
    using SignalHandler = void (*)(int);
    SignalHandler previous_handler_{nullptr};
};

// Runs the CLI without terminating the process. argv must include the program name.
[[nodiscard]] int run(
    const std::vector<std::string>& argv,
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    InterruptFlag& interrupt);

} // namespace revdash::cli
