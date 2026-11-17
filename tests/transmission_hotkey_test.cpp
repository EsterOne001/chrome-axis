#include "manual_transmission_input.hpp"

#include <iostream>

namespace {

using chrome_axis::ManualTransmissionInput;
using chrome_axis::PhysicalModeKey;
using chrome_axis::TransmissionSource;

constexpr unsigned int shortcut = 'L';

bool togglesWithPolling(TransmissionSource source)
{
    PhysicalModeKey key(shortcut);
    ManualTransmissionInput transmission;
    const bool simulation = source == TransmissionSource::keyboard;
    const bool hardware = source == TransmissionSource::hPattern;
    const bool paddles = source == TransmissionSource::paddles;
    const auto frame = [&](bool down, std::uint64_t tick) {
        key.setContext(true, tick);
        key.sample(shortcut, down, tick);
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
    key.sample(shortcut, false, 100);
    key.update(shortcut, true, false, 101);
    key.sample(shortcut, false, 102); // The hook sees key-down before Windows publishes the new level.
    if (key.takePress(shortcut, 102) != 101 || key.takePress(shortcut, 102))
        return false;
    key.sample(shortcut, true, 103);
    key.update(shortcut, false, false, 104);
    key.sample(shortcut, true, 105); // The asynchronous level still reports the previous key-down.
    if (key.takePress(shortcut, 105))
        return false;

    key.sample(shortcut, false, 106);
    key.sample(shortcut, true, 107);
    key.update(shortcut, true, false, 108); // Even a late callback cannot duplicate the sampled hold.
    if (key.takePress(shortcut, 108) != 107 || key.takePress(shortcut, 108))
        return false;
    key.update(shortcut, false, false, 109);
    // The listener still captures two taps entirely between vehicle updates.
    key.update(shortcut, true, false, 110);
    key.update(shortcut, false, false, 111);
    key.update(shortcut, true, false, 112);
    key.update(shortcut, false, false, 113);
    key.sample(shortcut, true, 114);
    key.sample(shortcut, false, 115);
    return key.takePress(shortcut, 115) == 110 && key.takePress(shortcut, 115) == 112 &&
        !key.takePress(shortcut, 115);
}

bool discardsStaleContextAndRearmsOnBindingChange()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    key.sample(shortcut, false, 100);
    key.sample(shortcut, true, 101);
    key.setContext(false, 0);
    key.sample(shortcut, false, 103);
    key.setContext(true, 104);
    key.sample(shortcut, true, 104);
    if (key.takePress(shortcut, 104))
        return false;

    key.select('F');
    key.sample(shortcut, false, 105);
    key.sample('F', true, 106);
    if (key.takePress('F', 106))
        return false;
    key.sample('F', false, 107);
    key.sample('F', true, 108);
    if (key.takePress('F', 108) != 108)
        return false;
    key.sample('F', false, 109);
    key.sample('F', true, 300); // Vehicle updates have stopped, so the context expired.
    return !key.takePress('F', 300);
}

bool rearmsAfterListenerStopsMidPress()
{
    PhysicalModeKey key(shortcut);
    key.setContext(true, 100);
    key.sample(shortcut, false, 100);
    key.update(shortcut, true, false, 101);
    if (key.takePress(shortcut, 101) != 101)
        return false;

    // Windows now reports the release while the listener misses both levels.
    for (const auto tick : {110u, 130u, 150u, 170u, 190u}) {
        key.setContext(true, tick);
        key.sample(shortcut, false, tick);
        if (key.takePress(shortcut, tick))
            return false;
    }
    key.sample(shortcut, true, 200);
    return key.takePress(shortcut, 200) == 200 && !key.takePress(shortcut, 200);
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
}
