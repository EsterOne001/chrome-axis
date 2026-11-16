#pragma once

#include <dinput.h>

#include <array>
#include <bitset>
#include <chrono>

namespace chrome_axis {

inline bool objectProbeDeferred(HRESULT result) noexcept {
    return result == E_PENDING || result == DIERR_NOTACQUIRED ||
        result == DIERR_OTHERAPPHASPRIO || result == DIERR_INPUTLOST ||
        result == static_cast<HRESULT>(DIERR_UNPLUGGED);
}

// A refused acquisition must not skip a fresh metadata probe. Some drivers
// briefly return E_PENDING even when the next GetObjectInfo works unacquired.
template <class Acquire, class Refresh, class Release>
void probeDeviceLayout(bool& needsAcquisition, Acquire&& acquire, Refresh&& refresh, Release&& release) {
    const bool acquired = needsAcquisition && acquire();
    needsAcquisition |= refresh();
    if (acquired)
        release();
}

template <std::size_t Count>
class DeviceObjectProbe {
public:
    using Clock = std::chrono::steady_clock;

    template <class Probe>
    void refresh(const std::bitset<Count>& wanted, Clock::time_point now, Probe&& probe) {
        const auto unknown = wanted & ~known_;
        if (unknown.none())
            return;
        for (std::size_t index = 0; index < Count; ++index) {
            if (unknown[index])
                refreshOne(index, now, probe);
        }
    }

    template <class Probe>
    bool refreshOne(std::size_t index, Clock::time_point now, Probe&& probe) {
        if (known_[index] || now < retryAt_[index])
            return false;
        const auto [result, present, completedAt] = probe(index);
        if (SUCCEEDED(result) || result == DIERR_OBJECTNOTFOUND) {
            known_.set(index);
            present_.set(index, SUCCEEDED(result) && present);
            deferred_.reset(index);
        } else {
            // Another driver error can follow a refused acquisition. Keep
            // the fallback eligible until this connection confirms the layout.
            if (objectProbeDeferred(result))
                deferred_.set(index);
            retryAt_[index] = completedAt + std::chrono::milliseconds(500);
        }
        return true;
    }

    const std::bitset<Count>& present() const noexcept { return present_; }
    const std::bitset<Count>& deferred() const noexcept { return deferred_; }
    std::bitset<Count> pending() const noexcept { return ~known_; }

private:
    std::bitset<Count> known_;
    std::bitset<Count> present_;
    std::bitset<Count> deferred_;
    std::array<Clock::time_point, Count> retryAt_{};
};

} // namespace chrome_axis
