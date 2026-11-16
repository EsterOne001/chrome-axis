#include "manual_transmission_input.hpp"

#include <iostream>

using chrome_axis::ManualTransmissionInput;

namespace {

bool hardwareModeNeedsAValidLever()
{
    ManualTransmissionInput input;
    if (input.update(true, {}, false, false, false, false, {}).manual)
        return false;
    const auto neutral = input.update(true, -1, false, false, false, false, {});
    const auto third = input.update(true, 3, false, false, false, false, {});
    if (!neutral.manual || neutral.gear != -1 || !third.manual || third.gear != 3)
        return false;
    if (input.update(true, 3, false, true, false, false, {}).manual ||
        input.update(true, 3, false, true, false, false, {}).manual)
        return false;
    input.suspend();
    if (input.update(true, {}, false, true, false, false, {}).manual ||
        input.update(true, -1, false, false, false, false, {}).manual)
        return false;
    const auto manual = input.update(true, -1, false, true, false, false, {});
    return manual.manual && manual.gear == -1 &&
        !input.update(true, {}, false, false, false, false, {}).manual &&
        input.update(true, -1, false, false, false, false, {}).manual;
}

bool simulationRequiresAnExplicitToggle()
{
    ManualTransmissionInput input;
    if (input.update(false, {}, true, true, true, false, {}).manual)
        return false;
    input.update(false, {}, true, false, false, false, {});
    const auto manual = input.update(false, {}, true, true, false, false, {});
    const auto next = input.update(false, {}, true, false, true, false, {});
    const auto held = input.update(false, {}, true, false, true, false, {});
    const auto direct = input.update(false, {}, true, false, false, true, 3);
    const auto neutral = input.update(false, {}, true, false, false, false, -1);
    const auto neutralAfterRelease = input.update(false, {}, true, false, false, false, {});
    if (!manual.manual || next.delta != 1 || held.delta != 0 || direct.gear != 3 ||
        direct.delta != 0 || neutral.gear != -1 || neutralAfterRelease.gear != -1)
        return false;
    input.suspend();
    const auto resumed = input.update(false, {}, true, true, true, false, 3);
    const auto released = input.update(false, {}, true, false, false, false, {});
    const auto previous = input.update(false, {}, true, false, false, true, {});
    return resumed.manual && !resumed.delta && !resumed.gear &&
        released.manual && previous.delta == -1 &&
        !input.update(false, {}, false, false, false, false, {}).manual;
}

} // namespace

int main()
{
    if (!hardwareModeNeedsAValidLever()) {
        std::cerr << "H-pattern did not respect the mode toggle or lever readiness\n";
        return 1;
    }
    if (!simulationRequiresAnExplicitToggle()) {
        std::cerr << "Transmission simulation used a held or disabled command\n";
        return 1;
    }
}
