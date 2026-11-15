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

constexpr int requestedGear(int observed, int availableEntries, int delta) noexcept {
    const int maximum = availableEntries - 1; // Vehicle records contain the reverse ratio.
    const int target = observed + delta;
    if ((delta != 1 && delta != -1) || observed < 1 || observed > maximum ||
        target < 1 || target > maximum)
        return 0;
    return target;
}

} // namespace chrome_axis