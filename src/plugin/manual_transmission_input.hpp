#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <optional>

namespace chrome_axis {

class PhysicalModeKey {
public:
    explicit PhysicalModeKey(unsigned int key) noexcept : state_(pack(key, Phase::initial)) {}

    unsigned int key() const noexcept { return state_.load(std::memory_order_relaxed) >> phaseBits; }

    void select(unsigned int key) noexcept {
        if (key != this->key())
            state_.store(pack(key, Phase::initial), std::memory_order_relaxed);
    }

    bool needsInitialState(unsigned int key) const noexcept {
        return state_.load(std::memory_order_relaxed) == pack(key, Phase::initial);
    }

    // Sample only whether the new key is up. Never infer a press from Windows' merged key state.
    void primeReleased(unsigned int key) noexcept {
        auto initial = pack(key, Phase::initial);
        state_.compare_exchange_strong(initial, pack(key, Phase::released), std::memory_order_relaxed);
    }

    void update(unsigned int key, bool pressed, bool injected) noexcept {
        if (injected)
            return;
        auto state = state_.load(std::memory_order_relaxed);
        while ((state >> phaseBits) == key) {
            const auto old = phase(state);
            const bool armed = old == Phase::released || old == Phase::pressed;
            const auto next = !pressed ? Phase::released : armed ? Phase::pressed : Phase::waitingForRelease;
            if (state_.compare_exchange_weak(state, pack(key, next), std::memory_order_relaxed))
                return;
        }
    }

    std::optional<bool> down(unsigned int key) const noexcept {
        const auto state = state_.load(std::memory_order_relaxed);
        if ((state >> phaseBits) != key)
            return std::nullopt;
        switch (phase(state)) {
        case Phase::released: return false;
        case Phase::pressed: return true;
        default: return std::nullopt;
        }
    }

private:
    enum class Phase : unsigned int { initial, waitingForRelease, released, pressed };
    static constexpr unsigned int phaseBits = 2;
    static constexpr unsigned int phaseMask = (1u << phaseBits) - 1;

    static constexpr unsigned int pack(unsigned int key, Phase phase) noexcept {
        return (key << phaseBits) | static_cast<unsigned int>(phase);
    }

    static constexpr Phase phase(unsigned int state) noexcept {
        return static_cast<Phase>(state & phaseMask);
    }

    std::atomic<unsigned int> state_;
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
                              bool sequentialArmed = false, int nativeDelta = 0) noexcept {
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
        bool toggledByKey = false;
        if (!toggle) {
            toggleArmed_ = false;
        } else if (!*toggle) {
            toggleArmed_ = true;
        } else if (toggleArmed_) {
            toggleArmed_ = false;
            manualSelected_ = !manualSelected_;
            toggledByKey = true;
        }

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
