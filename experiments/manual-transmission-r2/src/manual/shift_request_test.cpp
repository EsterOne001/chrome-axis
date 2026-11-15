#include "shift_request.hpp"

#include <cstdio>

int main() {
    using chrome_axis::requestedGear;
    static_assert(requestedGear(2, 7, 1) == 3);
    static_assert(requestedGear(2, 7, -1) == 1);
    static_assert(requestedGear(1, 7, -1) == 0);
    static_assert(requestedGear(6, 7, 1) == 0);
    static_assert(requestedGear(0, 7, 1) == 0);
    static_assert(requestedGear(-1, 7, 1) == 0);
    static_assert(requestedGear(4, -1, 1) == 0);
    static_assert(requestedGear(3, 7, 0) == 0);

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