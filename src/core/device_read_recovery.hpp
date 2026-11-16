#pragma once

#include <chrono>
#include <utility>

namespace chrome_axis {

class DeviceReadRecovery {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto reopenDelay = std::chrono::milliseconds(500);

    struct Pending {
        Clock::duration elapsed;
        bool first;
        bool reopen;
    };

    Pending pending(Clock::time_point now) noexcept {
        const bool first = pendingSince_ == Clock::time_point{};
        if (first)
            pendingSince_ = now;
        const auto elapsed = now - pendingSince_;
        const bool reopen = elapsed >= reopenDelay;
        awaitingReport_ |= reopen;
        return {elapsed, first, reopen};
    }

    void failed(bool disconnected) noexcept {
        pendingSince_ = {};
        awaitingReport_ |= disconnected;
    }

    bool accepted() noexcept {
        pendingSince_ = {};
        return std::exchange(awaitingReport_, false);
    }

private:
    Clock::time_point pendingSince_{};
    bool awaitingReport_{};
};

} // namespace chrome_axis
