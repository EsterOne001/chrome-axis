#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace chrome_axis {

class PhysicalModeKey {
public:
    static constexpr std::size_t pressCapacity = 8;

    explicit PhysicalModeKey(unsigned int key) noexcept : key_(key) {}

    unsigned int key() const noexcept { return key_.load(std::memory_order_relaxed); }

    void select(unsigned int key) noexcept {
        if (key > 0xff)
            return;
        std::scoped_lock lock(mutex_);
        if (key != this->key()) {
            key_.store(key, std::memory_order_relaxed);
            rearm();
        }
    }

    void setContext(bool active, std::uint64_t now) noexcept {
        std::scoped_lock lock(mutex_);
        if (contextAt_ && (!active || !contextFresh(now)))
            rearm();
        contextAt_ = active ? now : 0;
    }

    std::optional<std::uint64_t> initialState(unsigned int key) const noexcept {
        std::scoped_lock lock(mutex_);
        if (key == this->key() && contextAt_ && phase_ == Phase::initial)
            return revision_;
        return std::nullopt;
    }

    // Windows' merged state can establish that the key is up. Only a physical event records a press.
    void primeReleased(std::uint64_t initial) noexcept {
        std::scoped_lock lock(mutex_);
        if (initial == revision_ && contextAt_ && phase_ == Phase::initial)
            phase_ = Phase::released;
    }

    void update(unsigned int key, bool pressed, bool injected, std::uint64_t tick) noexcept {
        if (injected)
            return;
        std::scoped_lock lock(mutex_);
        if (key != this->key() || !contextFresh(tick))
            return;

        if (!pressed) {
            phase_ = Phase::released;
        } else if (phase_ == Phase::initial) {
            phase_ = Phase::waitingForRelease;
        } else if (phase_ == Phase::released) {
            phase_ = Phase::pressed;
            discardExpired(tick);
            if (count_ < presses_.size()) {
                presses_[(head_ + count_) % presses_.size()] = tick;
                ++count_;
            }
        }
    }

    std::optional<std::uint64_t> takePress(unsigned int key, std::uint64_t now) noexcept {
        std::scoped_lock lock(mutex_);
        if (key != this->key() || !contextFresh(now))
            return std::nullopt;
        discardExpired(now);
        // A press can arrive in the interval between reading the clock and taking this lock.
        if (!count_ || presses_[head_] > now)
            return std::nullopt;
        const auto press = presses_[head_];
        pop();
        return press;
    }

private:
    enum class Phase { initial, waitingForRelease, released, pressed };
    static constexpr std::uint64_t contextLifetime = 100; // ms
    static constexpr std::uint64_t pressLifetime = 200; // ms

    bool contextFresh(std::uint64_t now) const noexcept {
        return contextAt_ && now >= contextAt_ && now - contextAt_ <= contextLifetime;
    }

    void rearm() noexcept {
        ++revision_;
        phase_ = Phase::initial;
        head_ = count_ = 0;
    }

    void pop() noexcept {
        head_ = (head_ + 1) % presses_.size();
        --count_;
    }

    void discardExpired(std::uint64_t now) noexcept {
        while (count_ && now >= presses_[head_] && now - presses_[head_] > pressLifetime)
            pop();
    }

    std::atomic<unsigned int> key_;
    mutable std::mutex mutex_;
    std::uint64_t contextAt_{};
    std::uint64_t revision_{};
    Phase phase_{Phase::initial};
    std::array<std::uint64_t, pressCapacity> presses_{};
    std::size_t head_{}, count_{};
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

enum class TransmissionSource { none, keyboard, hPattern, paddles };

constexpr TransmissionSource transmissionSource(bool simulation, bool hardware, bool sequential) noexcept {
    if (simulation)
        return TransmissionSource::keyboard;
    if (hardware)
        return TransmissionSource::hPattern;
    return sequential ? TransmissionSource::paddles : TransmissionSource::none;
}

// Used on the game thread. Hardware input is already debounced and rearmed by AccessoryReader.
class ManualTransmissionInput {
public:
    void selectSource(TransmissionSource source) noexcept {
        if (source != source_) {
            *this = {};
            source_ = source;
            manualSelected_ = source == TransmissionSource::hPattern || source == TransmissionSource::paddles;
        }
    }

    TransmissionIntent update(bool hardware, std::optional<int> hardwareGear, bool simulation,
                              std::optional<bool> toggle, bool up, bool down, DirectGearKeys keyboard,
                              unsigned int toggleKey = 0, bool sequential = false,
                              bool sequentialArmed = false, int nativeDelta = 0,
                              unsigned int physicalPresses = 0) noexcept {
        selectSource(transmissionSource(simulation, hardware, sequential));

        if (toggleKey != toggleKey_) {
            toggleKey_ = toggleKey;
            toggleArmed_ = false;
        }
        bool toggledByKey = physicalPresses && hasSource();
        if (toggledByKey || !toggle) {
            toggleArmed_ = false;
        } else if (!*toggle) {
            toggleArmed_ = true;
        } else if (toggleArmed_) {
            toggleArmed_ = false;
            toggledByKey = true;
        }
        if (toggledByKey && (!physicalPresses || physicalPresses % 2 != 0))
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
    bool hasSource() const noexcept { return source_ != TransmissionSource::none; }

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

    TransmissionSource source_{};
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
