#pragma once

#include <atomic>
#include <optional>

namespace chrome_axis {

class PhysicalModeKey {
public:
    void update(bool pressed, bool injected) noexcept {
        if (!injected)
            pressed_.store(pressed, std::memory_order_relaxed);
    }

    bool down() const noexcept { return pressed_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> pressed_{};
};

struct DirectGearKeys {
    std::optional<int> gear;
    bool ambiguous{};

    bool released() const noexcept { return !gear && !ambiguous; }
};

enum class PedalRouting { standard, reverse, suspended };

constexpr std::optional<bool> reversePedalGear(int engaged, int target, int entries) noexcept {
    if (entries < 2 || engaged < -1 || engaged >= entries)
        return std::nullopt;
    if (engaged == 0)
        return true;
    // The game can use reverse channels as soon as R is the target. Wait until R engages.
    if (target < -1 || target >= entries || target == 0)
        return std::nullopt;
    return false;
}

// Retain logical pedals across a manual-to-auto handoff while the game is still
// in R. After a lost observation, require fresh gear data and released pedals.
class TransmissionPedals {
public:
    PedalRouting update(bool manual, std::optional<bool> reverse, bool released) noexcept {
        tracking_ |= manual;
        if (!tracking_)
            return PedalRouting::standard;

        if (!reverse)
            waitingForRelease_ = true;
        if (!reverse || (waitingForRelease_ && !released))
            return PedalRouting::suspended;
        waitingForRelease_ = false;
        if (*reverse) {
            wasReverse_ = true;
            return PedalRouting::reverse;
        }

        if (!manual && wasReverse_ && !released)
            return PedalRouting::suspended;
        wasReverse_ = false;
        tracking_ = manual;
        return PedalRouting::standard;
    }

    bool tracking() const noexcept { return tracking_; }

private:
    bool tracking_{};
    bool waitingForRelease_{};
    bool wasReverse_{};
};

struct TransmissionIntent {
    bool manual{};
    int delta{};
    std::optional<int> gear;
};

// Used on the game thread. Hardware input is already debounced and rearmed by AccessoryReader.
class ManualTransmissionInput {
public:
    TransmissionIntent update(bool hardware, std::optional<int> hardwareGear, bool simulation,
                              std::optional<bool> toggle, bool up, bool down, DirectGearKeys keyboard) noexcept {
        if (hardware != hardware_ || simulation != simulation_) {
            *this = {};
            hardware_ = hardware;
            simulation_ = simulation;
            manualSelected_ = hardware;
        }

        if (!toggle) {
            toggleArmed_ = false;
        } else if (!*toggle) {
            toggleArmed_ = true;
        } else if (toggleArmed_) {
            toggleArmed_ = false;
            manualSelected_ = !manualSelected_;
        }

        const int delta = sequentialShift(up, down);
        if (hardware)
            return {manualSelected_ && hardwareGear.has_value(), 0, hardwareGear};
        if (!simulation || !manualSelected_) {
            directArmed_ = false;
            keyboardSelection_.reset();
            return {};
        }
        if (keyboard.released())
            directArmed_ = true;
        if (keyboard.ambiguous)
            directArmed_ = false;
        if (directArmed_ && keyboard.gear)
            keyboardSelection_ = keyboard.gear;
        if (!keyboard.released())
            return {true, 0, keyboardSelection_};
        if (delta)
            keyboardSelection_.reset();
        return {true, delta, keyboardSelection_};
    }

    void suspend() noexcept {
        toggleArmed_ = false;
        shiftArmed_ = false;
        directArmed_ = false;
        keyboardSelection_.reset();
        up_ = false;
        down_ = false;
    }

private:
    int sequentialShift(bool up, bool down) noexcept {
        if (!shiftArmed_ || (up && down)) {
            shiftArmed_ = !up && !down;
            up_ = down_ = false;
            return 0;
        }
        const int delta = static_cast<int>(up && !up_) - static_cast<int>(down && !down_);
        up_ = up;
        down_ = down;
        return delta;
    }

    bool hardware_{};
    bool simulation_{};
    bool manualSelected_{};
    bool toggleArmed_{};
    bool shiftArmed_{};
    bool directArmed_{};
    std::optional<int> keyboardSelection_;
    bool up_{};
    bool down_{};
};

} // namespace chrome_axis
