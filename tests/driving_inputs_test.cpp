#include "../src/plugin/driving_inputs.hpp"
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"

#include <atomic>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace chrome_axis;
using Clock = DeviceReadClock;

const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
const std::string pedals = "{22222222-2222-2222-2222-222222222222}";

struct TestDriver {
    struct State {
        std::atomic<unsigned int> slowAttempts{};
        std::atomic<unsigned int> slowFinishes{};
        std::atomic<unsigned int> opens{};
        std::atomic<bool> pressed{};
        std::atomic<bool> pedalConnected{true};
    };

    static inline State* state{};

    TestDriver(HWND, DirectInputDiagnostic, DirectInputReadObserver observer, void* observerContext)
        : observer(observer), observerContext(observerContext) {
        ++state->opens;
    }

    bool ready() const noexcept { return true; }

    std::optional<DeviceSample> sampleConfigured(const std::string& id, const DeviceObjectSelection& wanted,
                                                 DeviceReadStatus& status) {
        DeviceSample sample;
        sample.sampledAt = Clock::now();
        status = DeviceReadStatus::available;
        if (id == pedals) {
            if (!state->pedalConnected) {
                pedalFault = true;
                status = DeviceReadStatus::unavailable;
                return std::nullopt;
            }
            if (std::exchange(pedalFault, false)) {
                status = DeviceReadStatus::resumed;
                recovery = 43;
            }
            sample.recoveryToken = recovery;
            sample.axesPresent[0] = wanted.axes[0];
            sample.buttonsPresent = wanted.buttons;
            sample.axes[0] = state->pressed ? 1000 : 0;
            sample.buttons.set(0, state->pressed);
        } else {
            sample.axesPresent[0] = wanted.axes[0];
            if (wanted.axes[1] && state->slowAttempts < 2) {
                ++state->slowAttempts;
                std::this_thread::sleep_for(120ms);
                observer(observerContext, {DirectInputReadEventKind::slow, id, "GetObjectInfo axis", E_PENDING, 120000});
                sample.axesPending.set(1);
                ++state->slowFinishes;
            } else {
                sample.axesPresent[1] = wanted.axes[1];
            }
        }
        return sample;
    }

    DirectInputReadObserver observer{};
    void* observerContext{};
    bool pedalFault{};
    std::uint64_t recovery{};
};

template <class Action>
bool waitUntil(Action&& action)
{
    const auto until = Clock::now() + 2s;
    do {
        if (action())
            return true;
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < until);
    return false;
}

int checkIsolatedDiscovery(bool assigned)
{
    TestDriver::State fake;
    TestDriver::state = &fake;
    DrivingInputs<TestDriver> input(5ms, 100ms);
    std::array devices{DrivingInputDevice{wheel, {}}, DrivingInputDevice{pedals, {}}};
    devices[0].objects.axes.set(assigned ? 1 : 0);
    devices[1].objects.axes.set(0);
    devices[1].objects.buttons.set(0);
    const auto window = GetDesktopWindow();
    const auto configured = [&] { return input.configure(window, devices); };
    if (!configured())
        return 1;

    Profile profile;
    profile.steering = {wheel, assigned ? 1u : 0u, 0, -1000, 1000, 0.02f};
    profile.brake = {pedals, 0, 0, 0, 1000, 0.02f};
    ControlReader controls(profile);
    AccessoryReader accessories({{pedals, AccessoryKind::Button, 0, 0, 1, VK_SPACE}});
    ControlState current;
    bool key{};
    const auto poll = [&] {
        input.beginPoll(Clock::now());
        current = controls.poll(input);
        key = accessories.poll(input).test(VK_SPACE);
    };
    if (!waitUntil([&] { poll(); return current.brakeReady && accessories.ready(); }))
        return 2;
    fake.pressed = true;
    if (!waitUntil([&] { poll(); return current.values.brake == 1.0f && key; }))
        return 3;
    unsigned expiredWheel{};
    bool brakeInterrupted{};
    bool unexpectedExpiration{};
    const auto until = Clock::now() + 325ms;
    while (Clock::now() < until) {
        poll();
        brakeInterrupted |= !current.brakeReady || current.values.brake != 1.0f || !key;
        input.drainEvents([&](const auto& id, const auto& event) {
            if (event.kind == DirectInputReadEventKind::sampleExpired) {
                expiredWheel += id == wheel;
                unexpectedExpiration |= id != wheel;
            }
        });
        std::this_thread::sleep_for(1ms);
    }
    if (unexpectedExpiration || brakeInterrupted || fake.slowAttempts != (assigned ? 2u : 0u) ||
        fake.slowFinishes != fake.slowAttempts || expiredWheel != fake.slowAttempts || !current.steeringReady)
        return 4;

    fake.pedalConnected = false;
    if (!waitUntil([&] { poll(); return !current.brakeReady && !key; }) || !current.steeringReady)
        return 5;
    fake.pedalConnected = true;
    if (!waitUntil([&] { poll(); return input.recoveryToken(pedals) == 43; }) || current.brakeReady || key)
        return 6;
    fake.pressed = false;
    if (!waitUntil([&] { poll(); return current.brakeReady && accessories.ready(); }))
        return 7;
    const auto opens = fake.opens.load();
    input.pause();
    input.beginPoll(Clock::now());
    DeviceReadStatus status{};
    if (input.sample(pedals, {}, &status) || !configured())
        return 8;
    if (!waitUntil([&] { poll(); return input.recoveryToken(pedals) == 43; }) || fake.opens != opens)
        return 9;
    return 0;
}

int checkCoalescedArming()
{
    Profile profile;
    profile.brake = {pedals, 0, 0, 0, 1000, 0.02f};
    ControlReader controls(profile);
    AccessoryReader accessories({{pedals, AccessoryKind::Button, 0, 0, 1, VK_SPACE}});
    DeviceSample report;
    report.axesPresent[0] = true;
    report.buttonsPresent.set(0);
    report.sampledAt = Clock::now();
    report.sequence = 1;
    const std::array<const DeviceSample*, 3> axes{nullptr, nullptr, &report};
    const std::array<const DeviceSample*, 1> buttons{&report};
    for (int i = 0; i < 10; ++i) {
        if (controls.update(axes).brakeReady || accessories.update(buttons).test(VK_SPACE) || accessories.ready())
            return 10;
    }
    ++report.sequence;
    controls.update(axes);
    accessories.update(buttons);
    ++report.sequence;
    if (!controls.update(axes).brakeReady)
        return 11;
    accessories.update(buttons);
    return accessories.ready() ? 0 : 12;
}

} // namespace

int main()
{
    for (const auto result : {checkIsolatedDiscovery(false), checkIsolatedDiscovery(true), checkCoalescedArming()}) {
        if (result) {
            std::cerr << "Driving input check failed: " << result << '\n';
            return result;
        }
    }
}
