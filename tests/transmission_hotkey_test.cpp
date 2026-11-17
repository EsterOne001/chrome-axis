#include "manual_transmission_input.hpp"

#include <iostream>

namespace {

using chrome_axis::ManualTransmissionInput;
using chrome_axis::PhysicalModeKey;
using chrome_axis::TransmissionSource;

constexpr unsigned int shortcut = 'J';
enum class DrivingPause { none, explicitPause, expired };

void sampleLevel(PhysicalModeKey& key, unsigned int virtualKey, bool pressed, std::uint64_t tick)
{
    key.sample(virtualKey, pressed, tick, key.sampleRevision());
}

bool togglesWithPolling(TransmissionSource source)
{
    PhysicalModeKey key(shortcut);
    ManualTransmissionInput transmission;
    const bool simulation = source == TransmissionSource::keyboard;
    const bool hardware = source == TransmissionSource::hPattern;
    const bool paddles = source == TransmissionSource::paddles;
    const auto frame = [&](bool down, std::uint64_t tick) {
        key.setContext(true, tick);
        sampleLevel(key, shortcut, down, tick);
        return transmission.update(hardware, 1, simulation, false, false, {}, paddles, true, 0,
                                   key.takePress(shortcut, tick).has_value() ? 1 : 0);
    };
    // A key held while resuming driving cannot enter the transmission queue.
    const auto heldOnEntry = frame(true, 100);
    const auto released = frame(false, 102);
    const auto first = frame(true, 104);
    const auto held = frame(true, 106);
    frame(false, 108);
    const auto second = frame(true, 110);
    return !heldOnEntry.toggledByKey && !released.toggledByKey && heldOnEntry.manual == !simulation &&
        first.toggledByKey && first.manual == simulation && !held.toggledByKey && held.manual == first.manual &&
        second.toggledByKey && second.manual == !simulation;
}

bool mergesAsynchronousLevelsWithHookEvents()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    sampleLevel(key, shortcut, false, 100);
    key.update(shortcut, true, false, 101);
    sampleLevel(key, shortcut, false, 102); // The hook sees key-down before Windows publishes the new level.
    bool fromHook{};
    if (key.takePress(shortcut, 102, &fromHook) != 101 || !fromHook || key.takePress(shortcut, 102))
        return false;
    sampleLevel(key, shortcut, true, 103);
    key.update(shortcut, false, false, 104);
    sampleLevel(key, shortcut, true, 105); // The asynchronous level still reports the previous key-down.
    if (key.takePress(shortcut, 105))
        return false;

    sampleLevel(key, shortcut, false, 106);
    sampleLevel(key, shortcut, true, 107);
    key.update(shortcut, true, false, 108); // Even a late callback cannot duplicate the sampled hold.
    if (key.takePress(shortcut, 108, &fromHook) != 107 || fromHook || key.takePress(shortcut, 108))
        return false;
    key.update(shortcut, false, false, 109);
    // The listener still captures two taps entirely between vehicle updates.
    key.update(shortcut, true, false, 110);
    key.update(shortcut, false, false, 111);
    key.update(shortcut, true, false, 112);
    key.update(shortcut, false, false, 113);
    sampleLevel(key, shortcut, true, 114);
    sampleLevel(key, shortcut, false, 115);
    return key.takePress(shortcut, 115) == 110 && key.takePress(shortcut, 115) == 112 &&
        !key.takePress(shortcut, 115);
}

bool discardsStaleContextAndRearmsOnBindingChange()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    sampleLevel(key, shortcut, false, 100);
    sampleLevel(key, shortcut, true, 101);
    key.setContext(false, 0);
    sampleLevel(key, shortcut, false, 103);
    key.setContext(true, 104);
    sampleLevel(key, shortcut, true, 104);
    if (key.takePress(shortcut, 104))
        return false;

    key.select('F');
    sampleLevel(key, shortcut, false, 105);
    sampleLevel(key, 'F', true, 106);
    if (key.takePress('F', 106))
        return false;
    sampleLevel(key, 'F', false, 107);
    sampleLevel(key, 'F', true, 108);
    if (key.takePress('F', 108) != 108)
        return false;
    sampleLevel(key, 'F', false, 109);
    sampleLevel(key, 'F', true, 300); // Vehicle updates have stopped, so the context expired.
    return !key.takePress('F', 300);
}

bool ignoresLateDownForTheSameHold(bool simulation, DrivingPause pause)
{
    PhysicalModeKey key(shortcut);
    ManualTransmissionInput transmission;
    const bool hardware = !simulation;
    const auto frame = [&](bool down, std::uint64_t tick) {
        key.setContext(true, tick);
        sampleLevel(key, shortcut, down, tick);
        return transmission.update(hardware, -1, simulation, false, false, {}, false, false, 0,
                                   key.takePress(shortcut, tick).has_value() ? 1 : 0);
    };
    const auto initial = frame(false, 1000);
    key.update(shortcut, true, false, 1001);
    const auto first = frame(false, 1005);
    if (initial.manual != hardware || !first.toggledByKey || first.manual != simulation)
        return false;

    if (pause == DrivingPause::explicitPause)
        key.setContext(false, 0);
    const unsigned int offset = pause == DrivingPause::expired ? 150 : 0;
    for (const auto tick : {1030u, 1050u, 1085u}) {
        const auto stale = frame(false, tick + offset);
        if (stale.toggledByKey || stale.manual != first.manual)
            return false;
    }
    const auto late = frame(true, 1090 + offset);
    const auto continuedHold = frame(true, 1105 + offset);
    const auto released = frame(false, 1110 + offset);
    const auto second = frame(true, 1120 + offset);
    return !late.toggledByKey && late.manual == first.manual &&
        !continuedHold.toggledByKey && continuedHold.manual == first.manual &&
        !released.toggledByKey && released.manual == first.manual &&
        second.toggledByKey && second.manual == initial.manual;
}

bool ignoresSamplesCapturedBeforeHookTransitions()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    sampleLevel(key, shortcut, false, 100);
    const auto oldRevision = key.sampleRevision();
    key.update(shortcut, true, false, 101);
    key.update(shortcut, false, false, 102);
    key.sample(shortcut, false, 103, oldRevision); // This up sample predates both hook transitions.
    sampleLevel(key, shortcut, true, 104); // The async level has yet to catch up to the release.
    if (key.takePress(shortcut, 104) != 101 || key.takePress(shortcut, 104))
        return false;

    sampleLevel(key, shortcut, false, 105);
    sampleLevel(key, shortcut, true, 106);
    return key.takePress(shortcut, 106) == 106 && !key.takePress(shortcut, 106);
}

bool rearmsAfterListenerStopsMidPress()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    sampleLevel(key, shortcut, false, 100);
    key.update(shortcut, true, false, 101);
    if (key.takePress(shortcut, 101) != 101)
        return false;

    // With no hook release, a sampled up may still belong to the state before the hook-down.
    for (const auto tick : {110u, 130u, 150u, 170u, 190u}) {
        key.setContext(true, tick);
        sampleLevel(key, shortcut, false, tick);
        if (key.takePress(shortcut, tick))
            return false;
    }
    sampleLevel(key, shortcut, true, 200);
    if (key.takePress(shortcut, 200))
        return false;
    sampleLevel(key, shortcut, false, 210);
    sampleLevel(key, shortcut, true, 220);
    return key.takePress(shortcut, 220) == 220 && !key.takePress(shortcut, 220);
}

bool rearmsWhenActuallyReleasedOutsideDriving(bool gameInForeground)
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    sampleLevel(key, shortcut, false, 100);
    key.update(shortcut, true, false, 101);
    if (key.takePress(shortcut, 102) != 101)
        return false;

    if (gameInForeground)
        key.setContext(false, 0); // In-game menu, where the keyboard listener still sees the game in front.
    key.update(shortcut, false, false, 104, gameInForeground);
    key.update(shortcut, true, false, 105, gameInForeground);
    key.update(shortcut, false, false, 106, gameInForeground);
    key.setContext(true, 110);
    sampleLevel(key, shortcut, true, 111); // Delayed down from outside driving.
    if (key.takePress(shortcut, 111))
        return false;

    key.update(shortcut, true, false, 112);
    return key.takePress(shortcut, 112) == 112 && !key.takePress(shortcut, 112);
}

bool usesOriginalWindowsEventTime()
{
    using Key = PhysicalModeKey;
    if (Key::eventTimestamp(0x1'0000'0010ull, 0xffff'fff0u) != 0xffff'fff0ull ||
        Key::eventTimestamp(2000, 1800) != 1800 || Key::eventTimestamp(2000, 1799) ||
        Key::eventTimestamp(2000, 2001) || Key::eventTimestamp(12, 0xffff'fff0u))
        return false;

    Key key(shortcut);
    key.setContext(true, 2000);
    sampleLevel(key, shortcut, false, 2000);
    key.update(shortcut, true, false, Key::eventTimestamp(2010, 1950));
    sampleLevel(key, shortcut, false, 2012);
    sampleLevel(key, shortcut, true, 2015); // Async down is from the same ineligible press.
    if (key.takePress(shortcut, 2015))
        return false;

    key.update(shortcut, false, false, Key::eventTimestamp(2018, 2016));
    key.update(shortcut, true, false, Key::eventTimestamp(2025, 2021));
    bool fromHook{};
    return key.takePress(shortcut, 2025, &fromHook) == 2021 && fromHook && !key.takePress(shortcut, 2025);
}

} // namespace

int main()
{
    if (!togglesWithPolling(TransmissionSource::keyboard) || !togglesWithPolling(TransmissionSource::hPattern) ||
        !togglesWithPolling(TransmissionSource::paddles)) {
        std::cerr << "Key polling must toggle once per hold in all three driving modes\n";
        return 1;
    }
    if (!mergesAsynchronousLevelsWithHookEvents()) {
        std::cerr << "The hook and sampled state duplicated or lost a key edge\n";
        return 2;
    }
    if (!discardsStaleContextAndRearmsOnBindingChange()) {
        std::cerr << "The shortcut did not rearm on focus or binding change\n";
        return 3;
    }
    if (!rearmsAfterListenerStopsMidPress()) {
        std::cerr << "The key could not rearm after the Windows listener stopped mid-press\n";
        return 4;
    }
    for (const auto pause : {DrivingPause::none, DrivingPause::explicitPause, DrivingPause::expired}) {
        if (!ignoresLateDownForTheSameHold(true, pause) || !ignoresLateDownForTheSameHold(false, pause)) {
            std::cerr << "One held J toggled twice when the Windows key state changed late\n";
            return 5;
        }
    }
    if (!ignoresSamplesCapturedBeforeHookTransitions()) {
        std::cerr << "An old sample overrode a more recent hook transition\n";
        return 6;
    }
    if (!rearmsWhenActuallyReleasedOutsideDriving(true) || !rearmsWhenActuallyReleasedOutsideDriving(false)) {
        std::cerr << "The shortcut must recognize a true release outside driving without queuing an inactive press\n";
        return 7;
    }
    if (!usesOriginalWindowsEventTime()) {
        std::cerr << "A delayed event crossed the context boundary or its Windows timestamp was misread\n";
        return 8;
    }
}
