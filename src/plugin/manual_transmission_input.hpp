#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace chrome_axis {

class PhysicalModeKey {
public:
    explicit PhysicalModeKey(unsigned int key) noexcept : state_(key & keyMask) {}

    unsigned int key() const noexcept { return keyOf(state_.load(std::memory_order_relaxed)); }

    void select(unsigned int key) noexcept {
        if (key > keyMask)
            return;
        auto state = state_.load(std::memory_order_relaxed);
        while (keyOf(state) != key) {
            if (state_.compare_exchange_weak(state, nextContext(state, key, eligible(state)), std::memory_order_relaxed))
                return;
        }
    }

    void setContext(bool active, std::uint64_t now) noexcept {
        const auto previous = contextAt_.exchange(active ? now : 0, std::memory_order_relaxed);
        if (!active || !contextFresh(previous, now))
            setEligible(false);
        if (active)
            setEligible(true);
    }

    std::optional<std::uint64_t> initialState(unsigned int key) const noexcept {
        const auto state = state_.load(std::memory_order_relaxed);
        if (keyOf(state) == key && eligible(state) && phaseOf(state) == Phase::initial)
            return state;
        return std::nullopt;
    }

    // Sample only whether the new key is up. Never infer a press from Windows' merged key state.
    void primeReleased(std::uint64_t initial) noexcept {
        state_.compare_exchange_strong(initial, withPhase(initial, Phase::released), std::memory_order_relaxed);
    }

    void update(unsigned int key, bool pressed, bool injected, std::uint64_t tick) noexcept {
        if (injected || !contextFresh(contextAt_.load(std::memory_order_relaxed), tick))
            return;
        auto state = state_.load(std::memory_order_relaxed);
        const auto revision = state & revisionMask;
        while (eligible(state) && keyOf(state) == key && (state & revisionMask) == revision) {
            const auto old = phaseOf(state);
            if (pressed && (old == Phase::pressed || old == Phase::waitingForRelease))
                return;
            const bool edge = pressed && old == Phase::released;
            const auto phase = !pressed ? Phase::released : edge ? Phase::pressed : Phase::waitingForRelease;
            auto next = withPhase(state, phase);
            if (edge)
                next = (next & ~pressMask) | pendingBit | ((tick & tickMask) << tickShift);
            if (state_.compare_exchange_weak(state, next, std::memory_order_relaxed))
                return;
        }
    }

    std::optional<std::uint64_t> takePress(unsigned int key, std::uint64_t now) noexcept {
        if (!contextFresh(contextAt_.load(std::memory_order_relaxed), now))
            return std::nullopt;
        auto state = state_.load(std::memory_order_relaxed);
        const auto revision = state & revisionMask;
        while (eligible(state) && keyOf(state) == key && (state & revisionMask) == revision && (state & pendingBit)) {
            const auto age = (now - (state >> tickShift)) & tickMask;
            // The hook can publish just after the caller reads the clock. Leave that press for the next update.
            if (age > tickMask / 2)
                return std::nullopt;
            if (state_.compare_exchange_weak(state, state & ~pressMask, std::memory_order_relaxed)) {
                return age <= pressLifetime && age <= now ? std::optional{now - age} : std::nullopt;
            }
        }
        return std::nullopt;
    }

private:
    enum class Phase : unsigned int { initial, waitingForRelease, released, pressed };
    static constexpr std::uint64_t contextLifetime = 100; // ms
    static constexpr std::uint64_t pressLifetime = 200; // ms
    // Keep the key, rearm state, context revision and its pending press in one atomic word.
    // The timestamp wraps after almost nine years; the revision detects concurrent rebinds and suspensions.
    static constexpr std::uint64_t keyMask = 0xff;
    static constexpr unsigned int phaseShift = 8;
    static constexpr std::uint64_t phaseMask = 3ull << phaseShift;
    static constexpr std::uint64_t eligibleBit = 1ull << 10;
    static constexpr std::uint64_t pendingBit = 1ull << 11;
    static constexpr std::uint64_t revisionStep = 1ull << 12;
    static constexpr unsigned int tickShift = 26;
    static constexpr std::uint64_t revisionMask = (1ull << tickShift) - revisionStep;
    static constexpr std::uint64_t tickMask = (1ull << (64 - tickShift)) - 1;
    static constexpr std::uint64_t pressMask = (tickMask << tickShift) | pendingBit;

    static constexpr unsigned int keyOf(std::uint64_t state) noexcept {
        return static_cast<unsigned int>(state & keyMask);
    }

    static constexpr bool eligible(std::uint64_t state) noexcept { return (state & eligibleBit) != 0; }

    static constexpr bool contextFresh(std::uint64_t last, std::uint64_t now) noexcept {
        return last && now >= last && now - last <= contextLifetime;
    }

    void setEligible(bool active) noexcept {
        auto state = state_.load(std::memory_order_relaxed);
        while (eligible(state) != active) {
            if (state_.compare_exchange_weak(state, nextContext(state, keyOf(state), active), std::memory_order_relaxed))
                return;
        }
    }

    static constexpr std::uint64_t nextContext(std::uint64_t state, unsigned int key, bool active) noexcept {
        const auto revision = ((state & revisionMask) + revisionStep) & revisionMask;
        return revision | key | (active ? eligibleBit : 0);
    }

    static constexpr std::uint64_t withPhase(std::uint64_t state, Phase phase) noexcept {
        return (state & ~phaseMask) | (static_cast<std::uint64_t>(phase) << phaseShift);
    }

    static constexpr Phase phaseOf(std::uint64_t state) noexcept {
        return static_cast<Phase>((state & phaseMask) >> phaseShift);
    }

    std::atomic<std::uint64_t> state_;
    std::atomic<std::uint64_t> contextAt_{};
};

struct DirectGearKeys {
    std::optional<int> gear;
    bool ambiguous{};

    bool released() const noexcept { return !gear && !ambiguous; }
};

// The USB reader can observe another press while the game finishes a shift.
class PaddleRequests {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    bool pending() const noexcept { return count_ != 0; }

    bool push(int delta, TimePoint sampled) noexcept {
        if (count_ == entries_.size())
            return false;
        entries_[(head_ + count_) % entries_.size()] = {delta, sampled};
        ++count_;
        return true;
    }

    int pop() noexcept {
        if (!pending())
            return 0;
        const int delta = entries_[head_].delta;
        head_ = (head_ + 1) % entries_.size();
        --count_;
        return delta;
    }

    bool expire(TimePoint now) noexcept {
        const auto previousCount = count_;
        while (pending() && now - entries_[head_].sampled >= std::chrono::milliseconds(900))
            pop();
        return previousCount != count_;
    }

    void clear() noexcept { count_ = 0; }

private:
    struct Entry {
        int delta{};
        TimePoint sampled{};
    };

    std::array<Entry, 4> entries_{};
    std::size_t head_{};
    std::size_t count_{};
};

class KeyboardClutchProbe {
public:
    static constexpr std::array positions{0.0f, 0.25f, 0.50f, 0.75f, 1.0f};

    std::optional<float> update(const std::array<bool, positions.size()>& keys) noexcept {
        std::optional<std::size_t> pressed;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (!keys[i])
                continue;
            if (pressed) {
                armed_ = false;
                return selected_;
            }
            pressed = i;
        }
        if (!pressed)
            armed_ = true;
        else if (armed_) {
            selected_ = positions[*pressed];
            armed_ = false;
        }
        return selected_;
    }

private:
    std::optional<float> selected_;
    bool armed_{};
};

class ClutchShiftAssist {
public:
    // A selector position stays pending while a paddle press is a momentary request.
    bool apply(std::optional<float> pressure, std::optional<int>& gear, int& delta) noexcept {
        constexpr float minimumClutchTravel = 0.65f;
        const bool released = pressure && *pressure >= minimumClutchTravel;
        const bool changingGear = gear && *gear >= 0 && gear != acceptedGear_;
        const bool blocked = !released && (changingGear || delta);
        if (blocked) {
            gear = acceptedGear_;
            delta = 0;
        } else {
            acceptedGear_ = gear;
        }
        return blocked;
    }

private:
    std::optional<int> acceptedGear_;
};

enum class PedalRouting { standard, reverse, suspended };
enum class PedalGear { unavailable, forwardOrNeutral, reverse, reversePending };

constexpr PedalGear observePedalGear(int engaged, int target, int entries, bool reverseIssued = false) noexcept {
    const bool engagedValid = entries >= 2 && engaged >= -1 && engaged < entries;
    if (engagedValid && engaged == 0)
        return PedalGear::reverse;
    // The game can use reverse channels before R engages.
    if (reverseIssued || (engagedValid && target == 0))
        return PedalGear::reversePending;
    if (!engagedValid || target < -1 || target >= entries)
        return PedalGear::unavailable;
    return PedalGear::forwardOrNeutral;
}

class TransmissionPedals {
public:
    PedalRouting update(bool manualRequested, PedalGear gear, bool released) noexcept {
        // A MANUAL request alone does not change the routing of an unobserved vehicle.
        if (!tracking_ && (!manualRequested || gear == PedalGear::unavailable))
            return PedalRouting::standard;
        tracking_ = true;

        if (gear == PedalGear::reversePending) {
            reversePossible_ = true;
            waitingForRelease_ = true;
            return PedalRouting::suspended;
        }
        if (gear == PedalGear::unavailable) {
            // In AUTO, a forward-only session can fall back after both pedals are released.
            if (!manualRequested && !reversePossible_ && released) {
                *this = {};
                return PedalRouting::standard;
            }
            waitingForRelease_ = true;
            return PedalRouting::suspended;
        }

        if (gear == PedalGear::reverse)
            reversePossible_ = true;
        if (!released && (waitingForRelease_ || (!manualRequested && gear != PedalGear::reverse && reversePossible_)))
            return PedalRouting::suspended;
        waitingForRelease_ = false;
        if (gear == PedalGear::reverse)
            return PedalRouting::reverse;

        reversePossible_ = false;
        tracking_ = manualRequested;
        return PedalRouting::standard;
    }

    bool tracking() const noexcept { return tracking_; }

private:
    bool tracking_{};
    bool waitingForRelease_{};
    bool reversePossible_{};
};

// Simulation does not emit accessory keys. A pending release from the USB
// source pauses polling so an injected key cannot toggle the transmission.
constexpr std::optional<bool> simulatedTransmissionKey(bool down, bool accessoryKeysPending) noexcept {
    return accessoryKeysPending ? std::nullopt : std::optional{down};
}

struct TransmissionIntent {
    bool manual{};
    int delta{};
    std::optional<int> gear;
    bool toggledByKey{};
};

// Used on the game thread. Hardware input is already debounced and rearmed by AccessoryReader.
class ManualTransmissionInput {
public:
    TransmissionIntent update(bool hardware, std::optional<int> hardwareGear, bool simulation,
                              std::optional<bool> toggle, bool up, bool down, DirectGearKeys keyboard,
                              unsigned int toggleKey = 0, bool sequential = false,
                              bool sequentialArmed = false, int nativeDelta = 0,
                              bool physicalTogglePressed = false) noexcept {
        if (hardware != hardware_ || simulation != simulation_ || sequential != sequential_) {
            *this = {};
            hardware_ = hardware;
            simulation_ = simulation;
            sequential_ = sequential;
            manualSelected_ = hardware || sequential;
        }

        if (toggleKey != toggleKey_) {
            toggleKey_ = toggleKey;
            toggleArmed_ = false;
        }
        bool toggledByKey = physicalTogglePressed && (simulation || hardware || sequential);
        if (toggledByKey || !toggle) {
            toggleArmed_ = false;
        } else if (!*toggle) {
            toggleArmed_ = true;
        } else if (toggleArmed_) {
            toggleArmed_ = false;
            toggledByKey = true;
        }
        if (toggledByKey)
            manualSelected_ = !manualSelected_;

        const int delta = sequentialShift(up, down);
        if (hardware)
            return {manualSelected_ && hardwareGear.has_value(), 0, hardwareGear, toggledByKey};
        if (sequential)
            return {manualSelected_ && sequentialArmed, manualSelected_ && sequentialArmed ? nativeDelta : 0,
                    std::nullopt, toggledByKey};
        if (!simulation || !manualSelected_) {
            directArmed_ = false;
            keyboardSelection_.reset();
            return {.gear = std::nullopt, .toggledByKey = toggledByKey};
        }
        if (keyboard.released())
            directArmed_ = true;
        if (keyboard.ambiguous)
            directArmed_ = false;
        if (directArmed_ && keyboard.gear)
            keyboardSelection_ = keyboard.gear;
        if (!keyboard.released())
            return {true, 0, keyboardSelection_, toggledByKey};
        if (delta)
            keyboardSelection_.reset();
        return {true, delta, keyboardSelection_, toggledByKey};
    }

    bool manualSelected() const noexcept { return manualSelected_; }

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
    bool sequential_{};
    bool simulation_{};
    bool manualSelected_{};
    unsigned int toggleKey_{};
    bool toggleArmed_{};
    bool shiftArmed_{};
    bool directArmed_{};
    std::optional<int> keyboardSelection_;
    bool up_{};
    bool down_{};
};

} // namespace chrome_axis
