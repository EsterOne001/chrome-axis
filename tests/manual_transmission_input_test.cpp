#include "manual_transmission_input.hpp"

#include <windows.h>

#include <iostream>

using chrome_axis::ManualTransmissionInput;
using chrome_axis::observePedalGear;
using chrome_axis::PedalGear;
using chrome_axis::PedalRelease;
using chrome_axis::PedalRouting;
using chrome_axis::PhysicalModeKey;
using chrome_axis::TransmissionPedals;

namespace {

static_assert(observePedalGear(0, -1, 8) == PedalGear::reverse);
static_assert(observePedalGear(-1, 0, 8) == PedalGear::reversePending);
static_assert(observePedalGear(2, -1, 8, false, 0) == PedalGear::reversePending);
static_assert(observePedalGear(-1, 1, 8) == PedalGear::neutralToForward);
static_assert(observePedalGear(-1, -1, 8, false, 1) == PedalGear::neutralToForward);
static_assert(observePedalGear(-1, -1, 8) == PedalGear::forwardOrNeutral);
static_assert(observePedalGear(-1, -2, 0) == PedalGear::unavailable);
static_assert(observePedalGear(-1, -2, 0, true) == PedalGear::reversePending);

constexpr PedalRelease bothReleased{true, true};

bool hardwareModeNeedsAValidLever()
{
    ManualTransmissionInput input;
    if (input.update(true, {}, false, false, false, {}).manual)
        return false;
    const auto neutral = input.update(true, -1, false, false, false, {});
    const auto third = input.update(true, 3, false, false, false, {});
    if (!neutral.manual || neutral.gear != -1 || !third.manual || third.gear != 3)
        return false;
    const auto automatic = input.update(true, 3, false, false, false, {}, false, false, 0, 1);
    if (automatic.manual || !automatic.toggledByKey || input.update(true, 3, false, false, false, {}).manual)
        return false;
    input.suspend();
    if (input.update(true, {}, false, false, false, {}).manual ||
        input.update(true, -1, false, false, false, {}).manual)
        return false;
    const auto manual = input.update(true, -1, false, false, false, {}, false, false, 0, 1);
    return manual.manual && manual.toggledByKey && manual.gear == -1 &&
        !input.update(true, {}, false, false, false, {}).manual &&
        input.update(true, -1, false, false, false, {}).manual;
}

bool simulationRequiresAnExplicitToggle()
{
    ManualTransmissionInput input;
    if (input.update(false, {}, true, true, false, {}).manual)
        return false;
    input.update(false, {}, true, false, false, {});
    const auto manual = input.update(false, {}, true, false, false, {}, false, false, 0, 1);
    const auto next = input.update(false, {}, true, true, false, {});
    const auto held = input.update(false, {}, true, true, false, {});
    const auto direct = input.update(false, {}, true, false, true, {.gear = 3});
    const auto neutral = input.update(false, {}, true, false, false, {.gear = -1});
    const auto neutralAfterRelease = input.update(false, {}, true, false, false, {});
    if (!manual.manual || !manual.toggledByKey || next.delta != 1 || held.delta != 0 || direct.gear != 3 ||
        direct.delta != 0 || neutral.gear != -1 || neutralAfterRelease.gear != -1)
        return false;
    input.suspend();
    const auto resumed = input.update(false, {}, true, true, false, {.gear = 3});
    const auto released = input.update(false, {}, true, false, false, {});
    const auto previous = input.update(false, {}, true, false, true, {});
    const auto automatic = input.update(false, {}, true, false, false, {}, false, false, 0, 1);
    return resumed.manual && !resumed.delta && !resumed.gear &&
        released.manual && previous.delta == -1 &&
        !automatic.manual && automatic.toggledByKey;
}

bool ambiguousKeysNeedFullRelease()
{
    ManualTransmissionInput input;
    input.update(false, {}, true, false, false, {}, false, false, 0, 1);
    input.suspend();
    const auto conflict = input.update(false, {}, true, false, false, {.ambiguous = true});
    const auto partlyReleased = input.update(false, {}, true, false, false, {.gear = 1});
    const auto released = input.update(false, {}, true, false, false, {});
    const auto selected = input.update(false, {}, true, false, false, {.gear = 1});
    const auto overlap = input.update(false, {}, true, false, false, {.ambiguous = true});
    const auto heldThird = input.update(false, {}, true, false, false, {.gear = 3});
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
        return input.update(true, -1, false, false, false, {},
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
        return input.update(true, -1, false, false, false, {},
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
    if (keyboard.takePress(VK_F8, 209) || keyboard.initialState(VK_F8))
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
    return !keyboard.takePress(VK_F8, 483) && !keyboard.initialState(VK_F8);
}

bool physicalPressSurvivesConcurrentContextRefresh()
{
    PhysicalModeKey keyboard{VK_F8};
    keyboard.setContext(true, 100);
    keyboard.update(VK_F8, false, false, 103);
    keyboard.setContext(true, 116);
    keyboard.update(VK_F8, true, false, 115);
    if (keyboard.takePress(VK_F8, 117) != 115ull)
        return false;

    keyboard.setContext(false, 118);
    keyboard.setContext(true, 121);
    keyboard.update(VK_F8, false, false, 122);
    keyboard.update(VK_F8, true, false, 119); // Delayed event from before this context.
    if (keyboard.takePress(VK_F8, 123))
        return false;
    keyboard.update(VK_F8, true, false, 124);
    return keyboard.takePress(VK_F8, 125) == 124ull;
}

bool consumesBothTapsAndClearsDisabledSource()
{
    using chrome_axis::TransmissionSource;
    PhysicalModeKey keyboard{VK_F8};
    keyboard.setContext(true, 100);
    keyboard.update(VK_F8, false, false, 101);
    keyboard.update(VK_F8, true, false, 106);
    keyboard.update(VK_F8, false, false, 112);
    keyboard.update(VK_F8, true, false, 158);
    keyboard.update(VK_F8, false, false, 165);
    if (keyboard.takePress(VK_F8, 168) != 106 || keyboard.takePress(VK_F8, 168) != 158 ||
        keyboard.takePress(VK_F8, 168))
        return false;

    ManualTransmissionInput input;
    input.selectSource(TransmissionSource::keyboard);
    const auto twoTaps = input.update(false, {}, true, false, false, {}, false, false, 0, 2);
    const auto oneTap = input.update(false, {}, true, false, false, {}, false, false, 0, 1);
    if (twoTaps.manual || !twoTaps.toggledByKey || !oneTap.manual)
        return false;
    input.suspend();
    input.selectSource(TransmissionSource::keyboard);
    if (!input.hasSource() || !input.manualSelected())
        return false;
    const auto disabled = input.update(false, {}, false, false, false, {}, false, false, 0, 1);
    return !disabled.manual && !disabled.toggledByKey && !input.hasSource() && !input.manualSelected();
}

bool nativePaddlesAndClutchRespectArming()
{
    ManualTransmissionInput transmission;
    const auto sequential = [&](bool ready, int edge, unsigned int presses = 0) {
        return transmission.update(false, {}, false, false, false, {}, true, ready, edge, presses);
    };
    if (sequential(false, 1).manual || sequential(true, 1).delta != 1 || sequential(true, 0).delta != 0 ||
        !sequential(true, -1).manual)
        return false;
    const auto automatic = sequential(true, 1, 1);
    const auto stayedAutomatic = sequential(true, 1);
    const auto manual = sequential(true, 1, 1);
    if (automatic.manual || automatic.delta || !automatic.toggledByKey || stayedAutomatic.manual ||
        stayedAutomatic.delta || !manual.manual || manual.delta != 1 || !manual.toggledByKey)
        return false;
    auto intent = transmission.update(true, -1, false, false, false, {});
    if (!intent.manual || intent.gear != -1)
        return false;

    chrome_axis::ClutchShiftAssist assist;
    int delta = 0;
    if (assist.apply({}, intent.gear, delta) || intent.gear != -1)
        return false;
    intent = transmission.update(true, 3, false, false, false, {});
    if (!assist.apply(0.649f, intent.gear, delta) || intent.gear != -1)
        return false;
    // The lever remains at the same position. The next report supplies that
    // level again and now reaches the clutch threshold without a new gesture.
    intent = transmission.update(true, 3, false, false, false, {});
    if (assist.apply(0.65f, intent.gear, delta) || intent.gear != 3)
        return false;
    intent.gear = -1;
    if (assist.apply({}, intent.gear, delta) || intent.gear != -1)
        return false;
    auto paddle = sequential(true, 1);
    if (!assist.apply({}, paddle.gear, paddle.delta) || paddle.delta != 0)
        return false;
    // Reaching the threshold cannot resurrect a consumed paddle press.
    paddle = sequential(true, 0);
    if (!chrome_axis::ClutchShiftAssist::pedalQualified(0.65f) ||
        assist.apply(0.65f, paddle.gear, paddle.delta) || paddle.delta != 0)
        return false;
    paddle = sequential(true, 1);
    if (assist.apply(0.65f, paddle.gear, paddle.delta) || paddle.delta != 1)
        return false;

    using namespace std::chrono_literals;
    chrome_axis::PaddleRequests queue;
    const auto at = chrome_axis::PaddleRequests::TimePoint{};
    if (!queue.push(1, at) || !queue.push(-1, at + 800ms) || !queue.expire(at + 900ms) || !queue.expired() ||
        queue.pop() != -1 || queue.pending() || queue.expired() || queue.pop() != 0)
        return false;
    if (!queue.push(1, at) || !queue.expire(at + 900ms) || !queue.expired() || queue.pop() != 0 ||
        !queue.expired() || !queue.push(1, at + 910ms) || queue.pop() != 1 || queue.expired())
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
    queue.discard();
    if (queue.pending() || !queue.expired())
        return false;
    queue.clear();
    if (queue.expired())
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
    if (pedals.update(true, PedalGear::unavailable, {}).routing != PedalRouting::standard ||
        pedals.update(false, PedalGear::unavailable, {}).routing != PedalRouting::standard ||
        pedals.update(false, PedalGear::unavailable, bothReleased).routing != PedalRouting::standard ||
        pedals.tracking())
        return false;

    return pedals.update(true, PedalGear::forwardOrNeutral, {}).routing == PedalRouting::standard &&
        pedals.update(true, PedalGear::unavailable, bothReleased).routing == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, {}).routing == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, bothReleased).routing == PedalRouting::standard &&
        !pedals.tracking() &&
        pedals.update(false, PedalGear::unavailable, {}).routing == PedalRouting::standard;
}

bool clutchInterlockRequiresAReadableRelease()
{
    // A zero-filled reading from a pending input and a fresh but held pedal
    // both lack release evidence when the clutch measurement becomes unavailable.
    using chrome_axis::clutchThrottleInterlock;
    const bool attenuating = clutchThrottleInterlock(false, 0.5f, false);
    const bool inputMissing = clutchThrottleInterlock(attenuating, std::nullopt, false);
    const bool heldAfterRecovery = clutchThrottleInterlock(inputMissing, std::nullopt, false);
    return attenuating && inputMissing && heldAfterRecovery &&
        !clutchThrottleInterlock(heldAfterRecovery, std::nullopt, true) &&
        !clutchThrottleInterlock(attenuating, 0.0f, false) &&
        !clutchThrottleInterlock(false, std::nullopt, false);
}

bool knownGearLetsEachPedalResumeIndependently()
{
    for (const bool brakeAvailable : {true, false}) {
        for (const int engaged : {3, -1}) {
            TransmissionPedals pedals;
            const auto known = observePedalGear(engaged, engaged, 8);
            const PedalRelease oneReleased{!brakeAvailable, brakeAvailable};
            if (pedals.update(true, known, {}).routing != PedalRouting::standard ||
                pedals.update(false, PedalGear::unavailable, {}).routing != PedalRouting::suspended ||
                pedals.update(false, PedalGear::unavailable, oneReleased).routing != PedalRouting::suspended)
                return false;

            const auto resumed = pedals.update(true, known, oneReleased);
            if (resumed.routing != PedalRouting::standard || resumed.brakeAllowed != brakeAvailable ||
                resumed.throttleAllowed == brakeAvailable)
                return false;

            // An absent pedal or one returning while held cannot clear its own latch.
            const auto held = pedals.update(true, known, {});
            if (held.routing != PedalRouting::standard || held.brakeAllowed != brakeAvailable ||
                held.throttleAllowed == brakeAvailable)
                return false;
            const auto recovered = pedals.update(true, known, {brakeAvailable, !brakeAvailable});
            if (recovered.routing != PedalRouting::standard || !recovered.brakeAllowed || !recovered.throttleAllowed)
                return false;
        }
    }
    return true;
}

bool retainsManualReversePedalsDuringHandoff()
{
    TransmissionPedals pedals;
    if (pedals.update(false, PedalGear::reverse, {}).routing != PedalRouting::standard ||
        pedals.update(true, PedalGear::reverse, {}).routing != PedalRouting::reverse ||
        pedals.update(false, PedalGear::reverse, {}).routing != PedalRouting::reverse ||
        pedals.update(false, PedalGear::forwardOrNeutral, {}).routing != PedalRouting::suspended ||
        pedals.update(false, PedalGear::forwardOrNeutral, bothReleased).routing != PedalRouting::standard ||
        pedals.update(false, PedalGear::reverse, {}).routing != PedalRouting::standard)
        return false;

    return pedals.update(true, PedalGear::forwardOrNeutral, {}).routing == PedalRouting::standard &&
        pedals.update(true, PedalGear::reversePending, {}).routing == PedalRouting::suspended &&
        pedals.update(false, PedalGear::unavailable, bothReleased).routing == PedalRouting::suspended &&
        pedals.update(true, PedalGear::reverse, {}).routing == PedalRouting::suspended &&
        pedals.update(true, PedalGear::reverse, bothReleased).routing == PedalRouting::reverse &&
        pedals.update(false, PedalGear::unavailable, bothReleased).routing == PedalRouting::suspended &&
        pedals.update(false, PedalGear::reverse, {}).routing == PedalRouting::suspended &&
        pedals.update(false, PedalGear::reverse, bothReleased).routing == PedalRouting::reverse &&
        pedals.update(false, PedalGear::neutralToForward, bothReleased).routing == PedalRouting::suspended &&
        pedals.update(true, PedalGear::neutralToForward, bothReleased).routing == PedalRouting::suspended &&
        pedals.update(true, PedalGear::forwardOrNeutral, {}).routing == PedalRouting::suspended &&
        pedals.update(true, PedalGear::forwardOrNeutral, bothReleased).routing == PedalRouting::standard;
}

bool automaticReverseKeepsStandardRoutingAfterPartialResume()
{
    for (const int engaged : {-1, 3}) {
        TransmissionPedals pedals;
        const auto known = observePedalGear(engaged, engaged, 8);
        pedals.update(true, known, bothReleased);
        pedals.update(false, PedalGear::unavailable, {});
        const auto resumed = pedals.update(false, known, {false, true});
        if (pedals.tracking() || resumed.routing != PedalRouting::standard ||
            resumed.throttleAllowed || !resumed.brakeAllowed)
            return false;

        for (const auto gear : {PedalGear::unavailable, PedalGear::reversePending, PedalGear::reverse}) {
            const auto automatic = pedals.update(false, gear, {});
            if (pedals.tracking() || automatic.routing != PedalRouting::standard ||
                automatic.throttleAllowed || !automatic.brakeAllowed)
                return false;
        }
        const auto recovered = pedals.update(false, PedalGear::reverse, {true, false});
        if (recovered.routing != PedalRouting::standard || !recovered.throttleAllowed || !recovered.brakeAllowed)
            return false;
    }
    return true;
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
    if (!physicalPressRequiresFreshDrivingContext() || !physicalPressSurvivesConcurrentContextRefresh() ||
        !consumesBothTapsAndClearsDisabledSource()) {
        std::cerr << "A physical shortcut lost an accepted press or accepted a stale driving context\n";
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
    if (!clutchInterlockRequiresAReadableRelease()) {
        std::cerr << "A missing throttle reading cleared the experimental clutch interlock\n";
        return 1;
    }
    if (!knownGearLetsEachPedalResumeIndependently()) {
        std::cerr << "A pedal resumed without its own release or was blocked by a missing pedal\n";
        return 1;
    }
    if (!retainsManualReversePedalsDuringHandoff() || !automaticReverseKeepsStandardRoutingAfterPartialResume()) {
        std::cerr << "Pedal routing changed before the reverse handoff was safe\n";
        return 1;
    }
}
