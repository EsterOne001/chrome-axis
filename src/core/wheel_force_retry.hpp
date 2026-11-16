#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace chrome_axis {

class WheelForceRetry {
public:
    using Clock = std::chrono::steady_clock;

    bool waiting(Clock::time_point now) const noexcept { return now < retryAt_; }

    void failed(Clock::time_point now) noexcept {
        retryAt_ = now + delay_;
        delay_ = std::min(delay_ * 2, std::chrono::seconds(30));
    }

    void succeeded() noexcept {
        retryAt_ = {};
        delay_ = std::chrono::seconds(2);
    }

    bool recovered(std::uint64_t token) noexcept {
        if (!token || token == recoveryToken_)
            return false;
        recoveryToken_ = token;
        succeeded();
        return true;
    }

private:
    Clock::time_point retryAt_{};
    std::chrono::seconds delay_{2};
    std::uint64_t recoveryToken_{};
};

} // namespace chrome_axis
