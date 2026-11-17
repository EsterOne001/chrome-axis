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
        if (!active) {
            if (contextAt_)
                rearm();
            contextStart_ = contextAt_ = 0;
            return;
        }
        if (now < contextAt_)
            return;
        if (!contextFresh(now)) {
            if (contextAt_)
                rearm();
            contextStart_ = now;
        }
        contextAt_ = now;
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
        // The hook may have read the clock just before the same context was refreshed.
        const auto sinceTick = contextAt_ > tick ? contextAt_ - tick : tick - contextAt_;
        if (key != this->key() || !contextAt_ || tick < contextStart_ || sinceTick > contextLifetime)
            return;
        if (pressed && (phase_ == Phase::initial || phase_ == Phase::released)) {
            sampleSync_ = sampleSync_ == SampleSync::awaitingUp ? SampleSync::awaitingUpThenDown : SampleSync::awaitingDown;
            sampledUpSince_.reset();
        } else if (!pressed && phase_ != Phase::released) {
            sampleSync_ = SampleSync::awaitingUp;
            sampledUpSince_.reset();
        }
        acceptLevel(pressed, tick);
    }

    void sample(unsigned int key, bool pressed, std::uint64_t now) noexcept {
        std::scoped_lock lock(mutex_);
        if (key != this->key() || !contextFresh(now))
            return;
        // The Windows hook sees an edge before GetAsyncKeyState reflects it.
        // Ignore the old level until the sample catches up with the hook.
        switch (sampleSync_) {
        case SampleSync::awaitingUp:
            if (pressed)
                return;
            break;
        case SampleSync::awaitingUpThenDown:
            if (pressed)
                return;
            sampleSync_ = SampleSync::awaitingDown;
            [[fallthrough]];
        case SampleSync::awaitingDown:
            if (!pressed) {
                // The listener may stop mid-press, with no sampled down and
                // no hook release. Recover after a sustained neutral level.
                if (!sampledUpSince_)
                    sampledUpSince_ = now;
                if (now - *sampledUpSince_ < sampleReleaseDelay)
                    return;
            }
            break;
        case SampleSync::matched:
            break;
        }
        sampleSync_ = SampleSync::matched;
        sampledUpSince_.reset();
        acceptLevel(pressed, now);
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
    enum class SampleSync { matched, awaitingDown, awaitingUp, awaitingUpThenDown };
    static constexpr std::uint64_t contextLifetime = 100; // ms
    static constexpr std::uint64_t pressLifetime = 200; // ms
    static constexpr std::uint64_t sampleReleaseDelay = 80; // ms

    bool contextFresh(std::uint64_t now) const noexcept {
        return contextAt_ && now >= contextAt_ && now - contextAt_ <= contextLifetime;
    }

    void acceptLevel(bool pressed, std::uint64_t tick) noexcept {
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

    void rearm() noexcept {
        ++revision_;
        phase_ = Phase::initial;
        sampleSync_ = SampleSync::matched;
        sampledUpSince_.reset();
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
    std::uint64_t contextStart_{};
    std::uint64_t contextAt_{};
    std::uint64_t revision_{};
    Phase phase_{Phase::initial};
    SampleSync sampleSync_{SampleSync::matched};
    std::optional<std::uint64_t> sampledUpSince_;
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
    bool expired() const noexcept { return expired_; }

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
        removeFront();
        expired_ = false;
        return delta;
    }

    bool expire(TimePoint now) noexcept {
        const auto previousCount = count_;
        while (pending() && now - entries_[head_].sampled >= std::chrono::milliseconds(900))
            removeFront();
        const bool discarded = previousCount != count_;
        expired_ |= discarded;
        return discarded;
    }

    void clear() noexcept {
        count_ = 0;
        expired_ = false;
    }

    void discard() noexcept {
        count_ = 0;
        expired_ = true;
    }

private:
    void removeFront() noexcept {
        head_ = (head_ + 1) % entries_.size();
        --count_;
    }

    struct Entry {
        int delta{};
        TimePoint sampled{};
    };

    std::array<Entry, 4> entries_{};
    std::size_t head_{};
    std::size_t count_{};
    bool expired_{};
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
    static bool pedalQualified(std::optional<float> pressure) noexcept {
        constexpr float minimumClutchTravel = 0.65f;
        return pressure && *pressure >= minimumClutchTravel;
    }

    // A selector position stays pending while a paddle press is a momentary request.
    bool apply(std::optional<float> pressure, std::optional<int>& gear, int& delta) noexcept {
        const bool changingGear = gear && *gear >= 0 && gear != acceptedGear_;
        const bool blocked = !pedalQualified(pressure) && (changingGear || delta);
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
enum class PedalGear { unavailable, forwardOrNeutral, neutralToForward, reverse, reversePending };

constexpr PedalGear observePedalGear(int engaged, int target, int entries, bool reverseIssued = false,
                                    std::optional<int> pending = std::nullopt) noexcept {
    const bool engagedValid = entries >= 2 && engaged >= -1 && engaged < entries;
    if (engagedValid && engaged == 0)
        return PedalGear::reverse;
    // The game can use reverse channels before R engages.
    if (reverseIssued || pending == 0 || (engagedValid && target == 0))
        return PedalGear::reversePending;
    if (!engagedValid || target < -1 || target >= entries)
        return PedalGear::unavailable;
    if (engaged == -1 && (target > 0 || (pending && *pending > 0 && *pending < entries)))
        return PedalGear::neutralToForward;
    return PedalGear::forwardOrNeutral;
}

struct PedalRelease {
    bool throttle{};
    bool brake{};
};

struct PedalRoutingDecision {
    PedalRouting routing{PedalRouting::standard};
    bool throttleAllowed{true};
    bool brakeAllowed{true};
};

class TransmissionPedals {
public:
    PedalRoutingDecision update(bool manualRequested, PedalGear gear, PedalRelease released) noexcept {
        constexpr PedalRoutingDecision suspended{PedalRouting::suspended, false, false};
        const bool bothReleased = released.throttle && released.brake;
        // A MANUAL request alone does not change the routing of an unobserved vehicle.
        if (!tracking_ && (!manualRequested || gear == PedalGear::unavailable))
            return standard(released);
        tracking_ = true;

        if (gear == PedalGear::reversePending) {
            reversePossible_ = true;
            throttleMustRelease_ = brakeMustRelease_ = true;
            return suspended;
        }
        if (gear == PedalGear::unavailable) {
            // In AUTO, a forward-only session can fall back after both pedals are released.
            if (!manualRequested && !reversePossible_ && bothReleased) {
                *this = {};
                return {};
            }
            throttleMustRelease_ = brakeMustRelease_ = true;
            return suspended;
        }

        if (gear == PedalGear::reverse) {
            reversePossible_ = true;
            if ((throttleMustRelease_ || brakeMustRelease_) && !bothReleased)
                return suspended;
            throttleMustRelease_ = brakeMustRelease_ = false;
            return {PedalRouting::reverse};
        }
        if (gear == PedalGear::neutralToForward && reversePossible_) {
            throttleMustRelease_ = brakeMustRelease_ = true;
            return suspended;
        }

        if (reversePossible_ && !manualRequested)
            throttleMustRelease_ = brakeMustRelease_ = true;
        reversePossible_ = false;
        tracking_ = manualRequested;
        return standard(released);
    }

    bool tracking() const noexcept { return tracking_; }

private:
    PedalRoutingDecision standard(PedalRelease released) noexcept {
        // A confirmed standard route lets each pedal prove its own release.
        // Keep these latches independent of ownership of AUTO pedal routing.
        throttleMustRelease_ &= !released.throttle;
        brakeMustRelease_ &= !released.brake;
        const auto routing = throttleMustRelease_ && brakeMustRelease_ ? PedalRouting::suspended : PedalRouting::standard;
        return {routing, !throttleMustRelease_, !brakeMustRelease_};
    }

    bool tracking_{};
    bool throttleMustRelease_{};
    bool brakeMustRelease_{};
    bool reversePossible_{};
};

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
                              bool up, bool down, DirectGearKeys keyboard, bool sequential = false,
                              bool sequentialArmed = false, int nativeDelta = 0,
                              unsigned int presses = 0) noexcept {
        selectSource(transmissionSource(simulation, hardware, sequential));

        const bool toggledByKey = presses != 0 && hasSource();
        if (toggledByKey && presses % 2 != 0)
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
    bool shiftArmed_{};
    bool directArmed_{};
    std::optional<int> keyboardSelection_;
    bool up_{};
    bool down_{};
};

} // namespace chrome_axis
