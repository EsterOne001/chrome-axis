#include "manual_transmission_input.hpp"

#include <windows.h>

#include <iostream>

using chrome_axis::ManualTransmissionInput;
using chrome_axis::PedalRouting;
using chrome_axis::PhysicalModeKey;
using chrome_axis::TransmissionPedals;

namespace {

static_assert(chrome_axis::reversePedalGear(0, -1, 8) == true);
static_assert(chrome_axis::reversePedalGear(-1, 0, 8) == std::nullopt);
static_assert(chrome_axis::reversePedalGear(-1, 1, 8) == false);
static_assert(chrome_axis::reversePedalGear(-1, -2, 0) == std::nullopt);

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
    const auto direct = input.update(false, {}, true, false, false, true, {.gear = 3});
    const auto neutral = input.update(false, {}, true, false, false, false, {.gear = -1});
    const auto neutralAfterRelease = input.update(false, {}, true, false, false, false, {});
    if (!manual.manual || next.delta != 1 || held.delta != 0 || direct.gear != 3 ||
        direct.delta != 0 || neutral.gear != -1 || neutralAfterRelease.gear != -1)
        return false;
    input.suspend();
    const auto resumed = input.update(false, {}, true, true, true, false, {.gear = 3});
    const auto released = input.update(false, {}, true, false, false, false, {});
    const auto previous = input.update(false, {}, true, false, false, true, {});
    return resumed.manual && !resumed.delta && !resumed.gear &&
        released.manual && previous.delta == -1 &&
        !input.update(false, {}, false, false, false, false, {}).manual;
}

bool ambiguousKeysNeedFullRelease()
{
    ManualTransmissionInput input;
    input.update(false, {}, true, false, false, false, {});
    input.update(false, {}, true, true, false, false, {});
    input.suspend();
    const auto conflict = input.update(false, {}, true, false, false, false, {.ambiguous = true});
    const auto partlyReleased = input.update(false, {}, true, false, false, false, {.gear = 1});
    const auto released = input.update(false, {}, true, false, false, false, {});
    const auto selected = input.update(false, {}, true, false, false, false, {.gear = 1});
    const auto overlap = input.update(false, {}, true, false, false, false, {.ambiguous = true});
    const auto heldThird = input.update(false, {}, true, false, false, false, {.gear = 3});
    return conflict.manual && !conflict.gear && !partlyReleased.gear && !released.gear
        && selected.gear == 1 && overlap.gear == 1 && heldThird.gear == 1;
}

bool injectedKeyDoesNotToggleTransmission()
{
    ManualTransmissionInput input;
    PhysicalModeKey keyboard{VK_F8};
    keyboard.primeReleased(VK_F8);
    const auto update = [&] { return input.update(true, -1, false, keyboard.down(VK_F8), false, false, {}).manual; };
    if (!update())
        return false;
    keyboard.update(VK_F8, true, true); // Sent by a held or pulsed USB accessory.
    if (!update())
        return false;
    keyboard.update(VK_F8, false, true);
    if (!update())
        return false;
    keyboard.update(VK_F8, true, false);
    if (update())
        return false;
    keyboard.update(VK_F8, false, true); // An injected release must not rearm the physical key.
    if (update())
        return false;
    keyboard.update(VK_F8, false, false);
    if (update())
        return false;
    keyboard.update(VK_F8, true, false);
    return update();
}

bool changingShortcutRearmsWithoutChangingModes()
{
    ManualTransmissionInput input;
    PhysicalModeKey keyboard{VK_F8};
    const auto update = [&] { return input.update(true, -1, false, keyboard.down(keyboard.key()), false, false, {}).manual; };
    keyboard.primeReleased(VK_F8);
    if (!update())
        return false;
    keyboard.select(VK_F9);
    if (!update() || keyboard.key() != VK_F9 || keyboard.down(VK_F8).has_value())
        return false;

    keyboard.update(VK_F9, true, false); // The new key was held during reassignment.
    keyboard.primeReleased(VK_F9);      // A stale sample claiming it was up must be discarded.
    if (keyboard.down(VK_F9).has_value() || !update())
        return false;
    keyboard.update(VK_F9, false, true);
    keyboard.update(VK_F8, false, false);
    if (keyboard.down(VK_F9).has_value() || !update())
        return false;

    keyboard.update(VK_F9, false, false);
    if (!update())
        return false;
    keyboard.update(VK_F9, true, false);
    if (update())
        return false;
    keyboard.select(VK_F9);             // Saving the same value must preserve a held key's state.
    return keyboard.down(VK_F9) == true && !update();
}

bool simulationPollsAfterAccessoryKeysAreReleased()
{
    ManualTransmissionInput input;
    const auto update = [&](bool keyDown, bool accessoryKeysPending, unsigned int key = VK_F8) {
        return input.update(false, {}, true, chrome_axis::simulatedTransmissionKey(keyDown, accessoryKeysPending),
                            false, false, {}, key);
    };
    // A key held when USB mode releases its last accessory must not arm the shortcut.
    if (update(true, true).manual || update(true, false).manual || update(false, false).manual)
        return false;
    const auto manual = update(true, false);
    const auto held = update(true, false);
    const auto released = update(false, false);
    const auto reboundWhileDown = update(true, false, VK_F9);
    const auto newKeyReleased = update(false, false, VK_F9);
    const auto automatic = update(true, false, VK_F9);
    return manual.manual && manual.toggledByKey && held.manual && !held.toggledByKey &&
        released.manual && !released.toggledByKey && reboundWhileDown.manual && !reboundWhileDown.toggledByKey &&
        newKeyReleased.manual && !newKeyReleased.toggledByKey && !automatic.manual && automatic.toggledByKey;
}

bool retainsManualReversePedalsDuringHandoff()
{
    TransmissionPedals pedals;
    if (pedals.update(false, true, false) != PedalRouting::standard ||
        pedals.update(true, true, false) != PedalRouting::reverse ||
        pedals.update(false, true, false) != PedalRouting::reverse ||
        pedals.update(false, false, false) != PedalRouting::suspended ||
        pedals.update(false, false, true) != PedalRouting::standard ||
        pedals.update(false, true, false) != PedalRouting::standard)
        return false;

    return pedals.update(true, false, false) == PedalRouting::standard &&
        pedals.update(true, chrome_axis::reversePedalGear(-1, 0, 8), false) == PedalRouting::suspended &&
        pedals.update(true, true, false) == PedalRouting::suspended &&
        pedals.update(true, true, true) == PedalRouting::reverse &&
        pedals.update(false, {}, false) == PedalRouting::suspended &&
        pedals.update(false, true, false) == PedalRouting::suspended &&
        pedals.update(false, true, true) == PedalRouting::reverse &&
        pedals.update(false, false, true) == PedalRouting::standard;
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
    if (!ambiguousKeysNeedFullRelease() || !injectedKeyDoesNotToggleTransmission() ||
        !changingShortcutRearmsWithoutChangingModes()) {
        std::cerr << "A held or injected key changed the transmission\n";
        return 1;
    }
    if (!simulationPollsAfterAccessoryKeysAreReleased()) {
        std::cerr << "Simulation did not rearm the shortcut after accessory keys were released\n";
        return 1;
    }
    if (!retainsManualReversePedalsDuringHandoff()) {
        std::cerr << "Pedal routing changed before the reverse handoff was safe\n";
        return 1;
    }
}
