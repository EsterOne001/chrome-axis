#pragma once

#include <dinput.h>

#include <array>
#include <bitset>
#include <chrono>

namespace chrome_axis {

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
            if (!unknown[index] || now < retryAt_[index])
                continue;
            const auto [result, present, completedAt] = probe(index);
            if (SUCCEEDED(result) || result == DIERR_OBJECTNOTFOUND) {
                known_.set(index);
                present_.set(index, SUCCEEDED(result) && present);
            } else {
                retryAt_[index] = completedAt + std::chrono::milliseconds(500);
            }
        }
    }

    const std::bitset<Count>& present() const noexcept { return present_; }
    std::bitset<Count> pending() const noexcept { return ~known_; }

private:
    std::bitset<Count> known_;
    std::bitset<Count> present_;
    std::array<Clock::time_point, Count> retryAt_{};
};

} // namespace chrome_axis
