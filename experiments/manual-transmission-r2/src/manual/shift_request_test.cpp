#include "shift_request.hpp"

#include <cstdio>

int main() {
    using chrome_axis::isSupportedForwardGear;
    using chrome_axis::requestedGear;
    static_assert(isSupportedForwardGear(1, 2));
    static_assert(isSupportedForwardGear(7, 8));
    static_assert(!isSupportedForwardGear(8, 8));
    static_assert(!isSupportedForwardGear(8, 9));
    static_assert(!isSupportedForwardGear(0, 8));
    static_assert(!isSupportedForwardGear(-1, 8));
    static_assert(requestedGear(6, 8, 1) == 7);
    static_assert(requestedGear(7, 8, 1) == 0);
    static_assert(requestedGear(7, 9, -1) == 0);
    static_assert(requestedGear(2, 7, 1) == 3);
    static_assert(requestedGear(2, 7, -1) == 1);
    static_assert(requestedGear(1, 7, -1) == 0);
    static_assert(requestedGear(6, 7, 1) == 0);
    static_assert(requestedGear(0, 7, 1) == 0);
    static_assert(requestedGear(-1, 7, 1) == 0);
    static_assert(requestedGear(4, -1, 1) == 0);
    static_assert(requestedGear(3, 7, 0) == 0);

    static_assert(requestedGear(0, 8, 1) == 0);
    static_assert(requestedGear(-1, 8, 1) == 0);
    static_assert(requestedGear(1, 0, 1) == 0);

    chrome_axis::ManualModeToggle mode;
    if (mode.active() || mode.update(true) || mode.update(false) ||
        !mode.update(true) || !mode.active() || mode.update(true) ||
        mode.update(false) || !mode.update(true) || mode.active()) {
        std::fputs("Manual mode selection edge failed\n", stderr);
        return 1;
    }
    if (mode.update(false) || !mode.update(true) || !mode.active()) {
        std::fputs("Manual mode reactivation failed\n", stderr);
        return 1;
    }
    mode = {};
    if (mode.active() || mode.update(true) || mode.update(false) ||
        !mode.update(true) || !mode.active()) {
        std::fputs("Manual mode reset or release rearm failed\n", stderr);
        return 1;
    }

    chrome_axis::ShiftKeys keys;
    if (keys.update(true, false) != 0 || keys.update(false, false) != 0 ||
        keys.update(true, false) != 1 || keys.update(true, false) != 0 ||
        keys.update(true, true) != 0 || keys.update(false, true) != 0 ||
        keys.update(false, false) != 0 || keys.update(false, true) != -1 ||
        keys.update(false, true) != 0 || keys.update(false, false) != 0) {
        std::fputs("Shift edges or neutral rearm failed\n", stderr);
        return 1;
    }
    keys = {};
    return keys.update(true, false) == 0 ? 0 : 1;
}