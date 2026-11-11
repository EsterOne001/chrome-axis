#pragma once

#include "chrome_axis/accessories.hpp"

namespace chrome_axis {

bool sameShifterInput(const AccessoryBinding& first, const AccessoryBinding& second) noexcept;

enum class ShifterCaptureStatus { AwaitingNeutral, AwaitingInput, Ambiguous, Duplicate, Captured };

struct ShifterCaptureResult {
    ShifterCaptureStatus status{};
    std::optional<AccessoryBinding> binding;
};

class ShifterCapture {
public:
    explicit ShifterCapture(std::string deviceId);

    // Call while the user holds the lever or paddles in their released position.
    bool captureNeutral(const DeviceSample& sample) noexcept;
    // Use after a skipped or interrupted position. The next capture requires a release.
    void resetPosition() noexcept;
    ShifterCaptureResult sample(const DeviceSample& sample,
                                std::span<const AccessoryBinding> existing = {});

private:
    struct DigitalState {
        std::bitset<buttonCount> buttons;
        std::array<std::uint16_t, povCount> povs{};

        bool operator==(const DigitalState&) const = default;
    };

    struct Signal {
        AccessoryKind kind{};
        std::size_t index{};
        long engaged{};

        bool operator==(const Signal&) const = default;
    };

    static DigitalState digitalState(const DeviceSample& sample) noexcept;
    void clearSignal() noexcept;

    std::string deviceId_;
    DigitalState neutral_{};
    std::optional<Signal> pending_;
    unsigned int neutralReports_{};
    unsigned int signalReports_{};
    bool neutralCaptured_{};
    bool armed_{};
};

} // namespace chrome_axis
