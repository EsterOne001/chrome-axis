#pragma once

namespace chrome_axis {

struct ShiftKeys {
    int update(bool up, bool down) noexcept {
        if (!released_) {
            released_ = !up && !down;
            return 0;
        }
        const bool upEdge = up && !up_;
        const bool downEdge = down && !down_;
        up_ = up;
        down_ = down;
        if (up && down) {
            released_ = false;
            up_ = false;
            down_ = false;
            return 0;
        }
        return static_cast<int>(upEdge) - static_cast<int>(downEdge);
    }

private:
    bool released_{};
    bool up_{};
    bool down_{};
};

constexpr bool isSupportedForwardGear(int gear, int availableEntries) noexcept {
    // The native array holds at most eight entries, including reverse at index zero.
    return availableEntries >= 2 && availableEntries <= 8 && gear >= 1 && gear < availableEntries;
}

constexpr int requestedGear(int observed, int availableEntries, int delta) noexcept {
    if ((delta != 1 && delta != -1) || !isSupportedForwardGear(observed, availableEntries))
        return 0;
    const int target = observed + delta;
    return isSupportedForwardGear(target, availableEntries) ? target : 0;
}

} // namespace chrome_axis