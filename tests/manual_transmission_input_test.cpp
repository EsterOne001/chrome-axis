#include "manual_transmission_input.hpp"

#include <windows.h>

#include <iostream>

using chrome_axis::ManualTransmissionInput;
using chrome_axis::PedalGear;
using chrome_axis::PedalRouting;
using chrome_axis::PhysicalModeKey;
using chrome_axis::TransmissionPedals;

namespace {

static_assert(chrome_axis::observePedalGear(0, -1, 8) == PedalGear::reverse);
static_assert(chrome_axis::observePedalGear(-1, 0, 8) == PedalGear::reversePending);
static_assert(chrome_axis::observePedalGear(-1, 1, 8) == PedalGear::forwardOrNeutral);
static_assert(chrome_axis::observePedalGear(-1, -2, 0) == PedalGear::unavailable);
static_assert(chrome_axis::observePedalGear(-1, -2, 0, true) == PedalGear::reversePending);

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
    keyboard.setContext(true, 100);
    const auto initial = keyboard.initialState(VK_F8);
    if (!initial)
        return false;
    keyboard.primeReleased(*initial);
    const auto update = [&](std::uint64_t tick) {
        return input.update(true, -1, false, std::nullopt, false, false, {}, VK_F8,
            false, false, 0, keyboard.takePress(VK_F8, tick).has_value());
    };
    if (!update(100).manual)
        return false;
    keyboard.update(VK_F8, true, true, 101); // Sent by a USB accessory.
    keyboard.update(VK_F8, false, true, 102);
    if (!update(104).manual)
        return false;
    keyboard.update(VK_F8, true, false, 105);
    keyboard.update(VK_F8, false, false, 107);
    const auto automatic = update(112); // The entire press was between game callbacks.
    if (automatic.manual || !automatic.toggledByKey || update(113).toggledByKey)
        return false;
    keyboard.update(VK_F8, true, false, 114);
    keyboard.update(VK_F8, true, false, 115); // Auto-repeat while held.
    keyboard.update(VK_F8, false, true, 116); // An injected release must not rearm.
    if (keyboard.takePress(VK_F8, 118) != 114ull)
        return false;
    keyboard.update(VK_F8, true, false, 119);
    if (keyboard.takePress(VK_F8, 120))
        return false;
    keyboard.update(VK_F8, false, false, 121);
    keyboard.update(VK_F8, true, false, 122);
    const auto manual = update(125);
    if (!manual.manual || !manual.toggledByKey || update(126).toggledByKey)
        return false;
    keyboard.update(VK_F8, false, false, 130);
    keyboard.update(VK_F8, true, false, 132);
    return !keyboard.takePress(VK_F8, 131) && keyboard.takePress(VK_F8, 133) == 132ull;
}

bool changingShortcutRearmsWithoutChangingModes()
{
    ManualTransmissionInput input;
    PhysicalModeKey keyboard{VK_F8};
    keyboard.setContext(true, 100);
    const auto update = [&](std::uint64_t tick) {
        const auto key = keyboard.key();
        return input.update(true, -1, false, std::nullopt, false, false, {}, key,
            false, false, 0, keyboard.takePress(key, tick).has_value()).manual;
    };
    keyboard.update(VK_F8, false, false, 101);
    keyboard.update(VK_F8, true, false, 102);
    if (update(103))
        return false;
    keyboard.select(VK_F9);
    if (update(104) || keyboard.key() != VK_F9 || keyboard.takePress(VK_F8, 104))
        return false;

    const auto initial = keyboard.initialState(VK_F9);
    if (!initial)
        return false;
    keyboard.update(VK_F9, true, false, 105); // A repeat from the key held during reassignment.
    keyboard.primeReleased(*initial);        // A stale sample claiming it was up must be discarded.
    if (update(106))
        return false;
    keyboard.update(VK_F9, false, true, 107);
    keyboard.update(VK_F8, false, false, 108);
    keyboard.update(VK_F9, true, false, 109);
    if (update(110))
        return false;

    keyboard.update(VK_F9, false, false, 111);
    keyboard.update(VK_F9, true, false, 112);
    keyboard.select(VK_F9); // Saving the same value must preserve the pending press.
    return update(113) && update(114);
}

bool physicalPressRequiresFreshDrivingContext()
{
    PhysicalModeKey keyboard{VK_F8};
    keyboard.setContext(true, 100);
    keyboard.update(VK_F8, false, false, 101);
    keyboard.update(VK_F8, true, false, 102);
    keyboard.update(VK_F8, false, false, 103);
    keyboard.setContext(false, 104);
    keyboard.update(VK_F8, true, false, 105);
    keyboard.setContext(true, 106);
    keyboard.update(VK_F8, true, false, 107); // Held while resuming.
    if (keyboard.takePress(VK_F8, 108))
        return false;
    keyboard.update(VK_F8, false, false, 109);
    keyboard.update(VK_F8, true, false, 207); // Game context has not refreshed in 101 ms.
    keyboard.setContext(true, 208);
    if (keyboard.takePress(VK_F8, 209) || !keyboard.initialState(VK_F8))
        return false;
    keyboard.update(VK_F8, false, false, 210);
    keyboard.update(VK_F8, true, false, 211);
    keyboard.setContext(true, 285);
    keyboard.setContext(true, 380);
    if (keyboard.takePress(VK_F8, 412)) // Press itself has aged beyond 200 ms.
        return false;
    keyboard.update(VK_F8, false, false, 413);
    keyboard.update(VK_F8, true, false, 414);
    if (keyboard.takePress(VK_F8, 481)) // A pending press cannot survive an expired context.
        return false;
    keyboard.setContext(true, 482);
    return !keyboard.takePress(VK_F8, 483) && keyboard.initialState(VK_F8).has_value();
}

bool physicalPressOverridesLevelArmingInSimulation()
{
    ManualTransmissionInput input;
    const auto update = [&](std::optional<bool> level, bool edge) {
        return input.update(false, {}, true, level, false, false, {}, VK_F8, false, false, 0, edge);
    };
    const auto enable = update(std::nullopt, true);
    const bool selected = input.manualSelected();
    const auto hold = update(true, false);
    const auto disable = update(std::nullopt, true);
    return enable.toggledByKey && enable.manual && selected && !hold.toggledByKey && hold.manual &&
        disable.toggledByKey && !disable.manual && !input.manualSelected();
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

bool nativePaddlesAndClutchRespectArming()
{
    ManualTransmissionInput transmission;
    const auto sequential = [&](bool ready, int edge) {
        return transmission.update(false, {}, false, false, false, false, {}, VK_F8, true, ready, edge);
    };
    if (sequential(false, 1).manual || sequential(true, 1).delta != 1 || sequential(true, 0).delta != 0 ||
        !sequential(true, -1).manual)
        return false;
    auto intent = transmission.update(true, -1, false, false, false, false, {}, VK_F8);
    if (!intent.manual || intent.gear != -1)
        return false;

    chrome_axis::ClutchShiftAssist assist;
    int delta = 0;
    if (assist.apply({}, intent.gear, delta) || intent.gear != -1)
        return false;
    intent.gear = 3;
    if (!assist.apply(0.6f, intent.gear, delta) || intent.gear != -1)
        return false;
    intent.gear = 3;
    if (assist.apply(0.7f, intent.gear, delta) || intent.gear != 3)
        return false;
    intent.gear.reset();
    delta = 1;
    if (!assist.apply({}, intent.gear, delta) || delta != 0)
        return false;

    using namespace std::chrono_literals;
    chrome_axis::PaddleRequests queue;
    const auto at = chrome_axis::PaddleRequests::TimePoint{};
    if (!queue.push(1, at) || !queue.push(-1, at + 800ms) || !queue.expire(at + 900ms) ||
        queue.pop() != -1 || queue.pending() || queue.pop() != 0)
        return false;
    for (const auto edge : {1, 1, -1, -1})
        if (!queue.push(edge, at))
            return false;
    if (queue.push(1, at) || queue.pop() != 1 || queue.pop() != 1 || queue.pop() != -1 || queue.pop() != -1)
        return false;
    queue.push(1, at);
    queue.clear();
    if (queue.pending())
        return false;

    chrome_axis::KeyboardClutchProbe probe;
    if (probe.update({false, false, true, false, false}) || probe.update({}))
        return false;
    if (probe.update({false, false, true, false, false}) != 0.50f)
        return false;
    probe.update({});
    if (probe.update({true, false, false, false, true}) != 0.50f ||
        probe.update({false, false, false, false, true}) != 0.50f)
        return false;
    probe.update({});
    return probe.update({true, false, false, false, false}) == 0.0f;
}

bool missingGearboxLeavesOrRestoresStandardPedals()
{
    TransmissionPedals pedals;
    if (pedals.update(true, PedalGear::unavailable, false) != PedalRouting::standard ||
        pedals.update(false, PedalGear::unavailable, false) != PedalRouting::standard ||
        pedals.update(false, PedalGear::unavailable, true) != PedalRouting::standard ||
        pedals.tracking())
        return false;

    return pedals.update(true, PedalGear::forwardOrNeutral, false) == PedalRouting::standard &&
        pedals.update(true, PedalGear::unavailable, true) == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, false) == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, true) == PedalRouting::standard &&
        !pedals.tracking() &&
        pedals.update(false, PedalGear::unavailable, false) == PedalRouting::standard;
}

bool retainsManualReversePedalsDuringHandoff()
{
    TransmissionPedals pedals;
    if (pedals.update(false, PedalGear::reverse, false) != PedalRouting::standard ||
        pedals.update(true, PedalGear::reverse, false) != PedalRouting::reverse ||
        pedals.update(false, PedalGear::reverse, false) != PedalRouting::reverse ||
        pedals.update(false, PedalGear::forwardOrNeutral, false) != PedalRouting::suspended ||
        pedals.update(false, PedalGear::forwardOrNeutral, true) != PedalRouting::standard ||
        pedals.update(false, PedalGear::reverse, false) != PedalRouting::standard)
        return false;

    return pedals.update(true, PedalGear::forwardOrNeutral, false) == PedalRouting::standard &&
        pedals.update(true, PedalGear::reversePending, false) == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, true) == PedalRouting::suspended &&
        pedals.update(true, PedalGear::reverse, false) == PedalRouting::suspended &&
        pedals.update(true, PedalGear::reverse, true) == PedalRouting::reverse &&
        pedals.update(false, PedalGear::unavailable, true) == PedalRouting::suspended &&
        pedals.update(false, PedalGear::reverse, false) == PedalRouting::suspended &&
        pedals.update(false, PedalGear::reverse, true) == PedalRouting::reverse &&
        pedals.update(false, PedalGear::forwardOrNeutral, true) == PedalRouting::standard;
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
    if (!physicalPressRequiresFreshDrivingContext() || !physicalPressOverridesLevelArmingInSimulation()) {
        std::cerr << "A physical shortcut lost an accepted press or accepted a stale driving context\n";
        return 1;
    }
    if (!simulationPollsAfterAccessoryKeysAreReleased()) {
        std::cerr << "Simulation did not rearm the shortcut after accessory keys were released\n";
        return 1;
    }
    if (!nativePaddlesAndClutchRespectArming()) {
        std::cerr << "Native transmission input or clutch did not respect rearming and gating\n";
        return 1;
    }
    if (!missingGearboxLeavesOrRestoresStandardPedals()) {
        std::cerr << "A missing gearbox observation stranded the pedals in AUTO\n";
        return 1;
    }
    if (!retainsManualReversePedalsDuringHandoff()) {
        std::cerr << "Pedal routing changed before the reverse handoff was safe\n";
        return 1;
    }
}
