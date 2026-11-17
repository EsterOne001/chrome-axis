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
        std::atomic<bool> blockWheel{};
    };

    static inline State* state{};

    TestDriver(HWND, DirectInputDiagnostic, DirectInputReadObserver observer, void* observerContext,
               DirectInputActiveCall* activeCall)
        : observer(observer), observerContext(observerContext), activeCall(activeCall) {
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
            if (state->blockWheel) {
                activeCall->store("GetDeviceState", std::memory_order_relaxed);
                state->blockWheel.wait(true);
                activeCall->store(nullptr, std::memory_order_relaxed);
            }
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
    DirectInputActiveCall* activeCall{};
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

int checkBlockedWorkerTelemetry()
{
    TestDriver::State fake;
    fake.blockWheel = true;
    TestDriver::state = &fake;
    DrivingInputs<TestDriver> input(5ms, 100ms);
    struct UnblockWheel {
        std::atomic<bool>& blocked;
        ~UnblockWheel() {
            blocked = false;
            blocked.notify_one();
        }
    } unblock{fake.blockWheel};

    std::array devices{DrivingInputDevice{wheel, {}}, DrivingInputDevice{pedals, {}}};
    for (auto& device : devices)
        device.objects.axes.set(0);
    if (!input.configure(GetDesktopWindow(), devices))
        return 48;

    unsigned int waitingEvents{};
    const auto poll = [&] {
        input.beginPoll(Clock::now());
        input.drainEvents([&](const std::string& id, const auto& event) {
            if (id == wheel && event.kind == DirectInputReadEventKind::slow &&
                std::string_view(event.operation.data()) == "Active GetDeviceState" &&
                event.durationMicros >= 250000)
                ++waitingEvents;
        });
    };
    if (!waitUntil([&] { poll(); return waitingEvents != 0; }))
        return 49;
    for (int i = 0; i < 10; ++i)
        poll();

    DeviceReadStatus wheelStatus{}, pedalStatus{};
    const auto pedal = input.sample(pedals, {}, &pedalStatus);
    const auto wheelSample = input.sample(wheel, {}, &wheelStatus);
    return waitingEvents == 1 && pedal && pedalStatus == DeviceReadStatus::available &&
        !wheelSample && wheelStatus == DeviceReadStatus::unavailable ? 0 : 50;
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

enum class Transition : unsigned int {
    neutral, shortPress, repeatedPress, overflow, hold, briefPending,
    paddleOverlap, thresholdJitter, oldOtherHold, freshOtherHold
};

struct TransitionDriver {
    struct State {
        std::atomic<Transition> scenario{Transition::neutral};
        std::array<std::atomic<std::uint64_t>, 2> progress{};
        bool sameDevicePaddles{};
    };

    static inline State* state{};

    TransitionDriver(HWND, DirectInputDiagnostic, DirectInputReadObserver, void*, DirectInputActiveCall*) {}
    bool ready() const noexcept { return true; }

    std::optional<DeviceSample> sampleConfigured(const std::string& id, const DeviceObjectSelection& wanted,
                                                 DeviceReadStatus& status) {
        status = DeviceReadStatus::available;
        DeviceSample sample;
        sample.sampledAt = Clock::now();
        sample.buttonsPresent = wanted.buttons;
        const auto scenario = state->scenario.load();
        if (scenario != previousScenario) {
            previousScenario = scenario;
            frame = 0;
        }
        ++frame;
        const auto deviceIndex = id == wheel ? 0 : 1;
        state->progress[deviceIndex] = (static_cast<std::uint64_t>(scenario) << 32) | frame;
        if (id == wheel) {
            if (scenario == Transition::briefPending && frame == 2) {
                status = DeviceReadStatus::pending;
                return std::nullopt;
            }
            const bool pressed =
                ((scenario == Transition::shortPress || scenario == Transition::briefPending ||
                  scenario == Transition::freshOtherHold) && frame == 1) ||
                (scenario == Transition::repeatedPress && (frame == 1 || frame == 3)) ||
                (scenario == Transition::overflow && frame <= 36 && frame % 2 == 1) ||
                scenario == Transition::hold || scenario == Transition::thresholdJitter ||
                scenario == Transition::paddleOverlap;
            sample.buttons.set(0, pressed);
            sample.buttons.set(1, pressed);
            if (state->sameDevicePaddles)
                sample.buttons.set(2, scenario == Transition::paddleOverlap && frame > 1);
            sample.axesPresent[0] = wanted.axes[0];
            sample.axes[0] = pressed ? 800 : 0;
            if (scenario == Transition::thresholdJitter)
                sample.axes[0] = frame % 2 == 0 ? 749 : 751;
            sample.povsPresent[0] = wanted.povs[0];
            sample.povs[0] = pressed ? 9000 : povCentered;
        } else {
            if (!state->sameDevicePaddles)
                sample.buttons.set(0, scenario == Transition::paddleOverlap && frame > 1);
            sample.buttons.set(1, scenario == Transition::oldOtherHold || scenario == Transition::freshOtherHold);
            // The other controller must not produce a fresh report until the
            // wheel has already sampled the tap.
            if (scenario == Transition::oldOtherHold ||
                (scenario == Transition::freshOtherHold &&
                 (state->progress[0].load() >> 32) != static_cast<unsigned int>(scenario)))
                sample.sampledAt -= 160ms;
        }
        return sample;
    }

    Transition previousScenario{Transition::neutral};
    unsigned int frame{};
};

int checkBufferedTransitions(bool sameDevicePaddles)
{
    TransitionDriver::State fake;
    fake.sameDevicePaddles = sameDevicePaddles;
    TransitionDriver::state = &fake;
    DrivingInputs<TransitionDriver> input(1ms, 300ms);
    const AccessoryProfile profile{
        {.deviceId = wheel, .kind = AccessoryKind::Button, .index = 0, .engaged = 1, .key = 'J',
         .activation = AccessoryActivation::pulse},
        {.deviceId = wheel, .kind = AccessoryKind::Button, .index = 1, .engaged = 1, .key = VK_SPACE},
        {.deviceId = wheel, .kind = AccessoryKind::Axis, .index = 0, .engaged = 1000, .key = 'E',
         .thresholds = {75, 40}, .activation = AccessoryActivation::pulse},
        {.deviceId = wheel, .kind = AccessoryKind::Pov, .index = 0, .rest = povCentered, .engaged = 9000,
         .key = 'K', .activation = AccessoryActivation::pulse},
        {.deviceId = wheel, .kind = AccessoryKind::Button, .index = 0, .engaged = 1,
         .role = ShifterRole::Upshift, .nativeSequential = true},
        {.deviceId = sameDevicePaddles ? wheel : pedals, .kind = AccessoryKind::Button,
         .index = sameDevicePaddles ? 2u : 0u, .engaged = 1,
         .role = ShifterRole::Downshift, .nativeSequential = true},
        {.deviceId = pedals, .kind = AccessoryKind::Button, .index = 1, .engaged = 1, .key = 'B'},
    };
    std::array devices{DrivingInputDevice{.id = wheel}, DrivingInputDevice{.id = pedals}};
    for (auto& device : devices) {
        device.discrete = std::make_shared<AccessoryProfile>();
        for (const auto& binding : profile)
            if (binding.deviceId == device.id)
                device.discrete->push_back(binding);
    }
    devices[0].objects.buttons.set(0).set(1).set(2);
    devices[0].objects.axes.set(0);
    devices[0].objects.povs.set(0);
    devices[1].objects.buttons.set(0).set(1);
    const auto window = GetDesktopWindow();
    if (!input.configure(window, devices))
        return 20;
    AccessoryReader accessories(profile);

    struct Collected {
        unsigned int buttonPresses{}, axisPresses{}, povPresses{}, shifts{};
        bool heldDuringReplay{}, heldCurrently{}, unrelatedStillHeld{}, armed{};
        Clock::time_point shiftAt{};
    };
    const auto poll = [&](Clock::time_point now = Clock::now()) {
        Collected collected;
        input.beginPoll(now);
        PaddleInputBatch paddleBatch;
        input.visitAccessoryDisruptions([&](std::string_view id, bool reset) {
            if (reset)
                accessories.resetDiscrete(id);
        });
        input.replayAccessories([&](bool latest) {
            AccessoryTransmissionSample shift;
            std::bitset<256> pulses;
            const auto held = accessories.poll(input, {}, &pulses, nullptr, nullptr, &shift, nullptr, !latest);
            collected.buttonPresses += pulses.test('J');
            collected.axisPresses += pulses.test('E');
            collected.povPresses += pulses.test('K');
            input.observePaddles(paddleBatch, shift, 100ms);
            if (latest) {
                collected.heldCurrently = held.test(VK_SPACE);
                collected.unrelatedStillHeld = held.test('B');
                collected.armed = shift.sequentialArmed;
            } else {
                collected.heldDuringReplay |= held.test(VK_SPACE);
            }
        });
        paddleBatch.finish();
        if (collected.armed) {
            collected.shifts = static_cast<unsigned int>(paddleBatch.edges().size());
            if (!paddleBatch.edges().empty())
                collected.shiftAt = paddleBatch.edges().back().second;
        }
        return collected;
    };
    const auto run = [&](Transition scenario, unsigned int frames) {
        fake.scenario = scenario;
        return waitUntil([&] {
            return std::ranges::all_of(fake.progress, [&](const auto& observed) {
                const auto progress = observed.load();
                return progress >> 32 == static_cast<unsigned int>(scenario) &&
                    static_cast<unsigned int>(progress) >= frames;
            });
        });
    };
    const auto armed = [&] { return waitUntil([&] { return poll().armed && accessories.ready(); }); };
    if (!armed())
        return 21;
    const auto started = Clock::now();
    if (!run(Transition::shortPress, 5))
        return 22;
    const auto single = poll();
    if (single.buttonPresses != 1 || single.axisPresses != 1 || single.povPresses != 1 || single.shifts != 1 ||
        !single.heldDuringReplay || single.heldCurrently || !single.armed || single.shiftAt < started ||
        poll().buttonPresses != 0)
        return 23;

    if (!run(Transition::repeatedPress, 6))
        return 24;
    const auto multiple = poll();
    if (multiple.buttonPresses != 2 || multiple.axisPresses != 2 || multiple.povPresses != 2 || multiple.shifts != 2)
        return 25;

    if (!run(Transition::briefPending, 6))
        return 37;
    const auto briefPending = poll();
    if (briefPending.buttonPresses != 1 || briefPending.shifts != 1)
        return 38;

    if (!run(Transition::paddleOverlap, 5))
        return 39;
    const auto overlap = poll();
    if (!overlap.armed || overlap.shifts != 0 || !run(Transition::neutral, 4) || poll().shifts != 0)
        return 40;

    if (!run(Transition::hold, 4))
        return 26;
    if (!poll().heldCurrently || !poll().heldCurrently)
        return 27;

    // The axis is still engaged until 40%, while individual reports cross 75%.
    if (!run(Transition::thresholdJitter, 22))
        return 41;
    const auto jitter = poll();
    int jitterOverflows = 0;
    input.drainEvents([&](const auto&, const auto& event) {
        jitterOverflows += event.kind == DirectInputReadEventKind::discreteOverflow;
    });
    if (jitterOverflows || !jitter.heldCurrently || !jitter.armed || jitter.axisPresses || jitter.shifts ||
        !run(Transition::neutral, 4) || poll().heldCurrently)
        return 42;

    // Keep the held button on the other device. Its historical report can
    // preserve that hold while being too old to validate a split-device shift.
    if (!run(Transition::oldOtherHold, 5) || !poll().unrelatedStillHeld ||
        !run(Transition::freshOtherHold, 5))
        return 43;
    const auto crossDevice = poll(Clock::now() + 155ms);
    if (!crossDevice.unrelatedStillHeld || crossDevice.buttonPresses != 1 ||
        crossDevice.shifts != (sameDevicePaddles ? 1u : 0u) ||
        !run(Transition::neutral, 4))
        return 44;
    poll();

    if (!run(Transition::overflow, 40))
        return 28;
    const auto overflow = poll();
    int overflowEvents = 0;
    input.drainEvents([&](const auto& id, const auto& event) {
        overflowEvents += id == wheel && event.kind == DirectInputReadEventKind::discreteOverflow;
    });
    if (overflow.buttonPresses || overflow.shifts || overflow.armed || overflowEvents != 1 || !armed())
        return 29;

    if (!run(Transition::shortPress, 5))
        return 30;
    input.discardAccessorySamples(); // A profile or vehicle change preserves healthy handles.
    accessories.reset();
    const auto discarded = poll();
    if (discarded.buttonPresses || discarded.shifts || discarded.heldDuringReplay || !armed())
        return 31;

    if (!run(Transition::repeatedPress, 6))
        return 32;
    input.pause();
    accessories.reset();
    if (!input.configure(window, devices) || !armed())
        return 33;
    const auto resumed = poll();
    if (resumed.buttonPresses || resumed.shifts || resumed.heldDuringReplay)
        return 34;

    // Expire the reports deterministically without slowing down the read workers.
    if (!run(Transition::shortPress, 5))
        return 35;
    const auto expired = poll(Clock::now() + 301ms);
    return expired.buttonPresses || expired.shifts || expired.heldDuringReplay || expired.armed ? 36 : 0;
}

enum class OpponentInterruption { none, noReport, disconnect };

struct SplitPaddleDriver {
    struct State {
        std::string_view actuated;
        OpponentInterruption interruption{};
        std::atomic<bool> press{}, opponentSilent{}, opponentWasSilent{};
        std::atomic<Clock::duration::rep> edgeAt{};
        std::atomic<unsigned int> reportsAfterRelease{}, opponentReportsSinceEdge{};
    };

    static inline State* state{};

    SplitPaddleDriver(HWND, DirectInputDiagnostic, DirectInputReadObserver, void*, DirectInputActiveCall*) {}
    bool ready() const noexcept { return true; }

    std::optional<DeviceSample> sampleConfigured(const std::string& id, const DeviceObjectSelection& wanted,
                                                 DeviceReadStatus& status) {
        const bool actuated = id == state->actuated;
        if (!actuated && state->opponentSilent) {
            state->opponentWasSilent = true;
            status = state->interruption == OpponentInterruption::disconnect
                ? DeviceReadStatus::unavailable : DeviceReadStatus::pending;
            return std::nullopt;
        }
        status = DeviceReadStatus::available;
        DeviceSample sample;
        sample.sampledAt = Clock::now();
        sample.buttonsPresent = wanted.buttons;
        if (actuated) {
            if (state->press) {
                sample.buttons.set(0);
                if (!state->edgeAt)
                    state->edgeAt = sample.sampledAt.time_since_epoch().count();
            } else if (state->edgeAt) {
                ++state->reportsAfterRelease;
            }
        } else {
            const auto edge = state->edgeAt.load();
            if (edge && sample.sampledAt.time_since_epoch().count() > edge)
                ++state->opponentReportsSinceEdge;
        }
        return sample;
    }
};

int checkSplitPaddleSampling(bool pressDown, OpponentInterruption interruption)
{
    SplitPaddleDriver::State fake{.actuated = pressDown ? pedals : wheel, .interruption = interruption};
    SplitPaddleDriver::state = &fake;
    DrivingInputs<SplitPaddleDriver> input(5ms, 100ms);
    const AccessoryProfile profile{
        {.deviceId = wheel, .kind = AccessoryKind::Button, .index = 0, .engaged = 1,
         .role = ShifterRole::Upshift, .nativeSequential = true},
        {.deviceId = pedals, .kind = AccessoryKind::Button, .index = 0, .engaged = 1,
         .role = ShifterRole::Downshift, .nativeSequential = true},
    };
    std::array devices{DrivingInputDevice{.id = wheel}, DrivingInputDevice{.id = pedals}};
    for (std::size_t i = 0; i < devices.size(); ++i) {
        devices[i].discrete = std::make_shared<AccessoryProfile>(AccessoryProfile{profile[i]});
        devices[i].objects.buttons.set(0);
    }
    if (!input.configure(GetDesktopWindow(), devices))
        return 48;

    AccessoryReader reader(profile);
    struct Result {
        bool armed{};
        Clock::time_point priorOpposite{};
        std::vector<std::pair<int, Clock::time_point>> shifts;
    };
    const auto consume = [&] {
        Result result;
        input.beginPoll(Clock::now());
        input.visitAccessoryDisruptions([&](const auto& id, bool reset) {
            if (reset)
                reader.resetDiscrete(id);
        });
        PaddleInputBatch batch;
        input.replayAccessories([&](bool latest) {
            AccessoryTransmissionSample sample;
            reader.poll(input, {}, nullptr, nullptr, nullptr, &sample, nullptr, !latest);
            if (sample.shiftDelta)
                result.priorOpposite = sample.oppositePaddleSampledAt;
            input.observePaddles(batch, sample, 100ms);
            if (latest)
                result.armed = sample.sequentialArmed;
        });
        batch.finish();
        result.shifts.assign(batch.edges().begin(), batch.edges().end());
        return result;
    };
    if (!waitUntil([&] { return consume().armed; }))
        return 49;

    // The consumer's previous report becomes stale while the worker either
    // continues to read the stationary paddle or really loses its signal.
    fake.opponentSilent = interruption != OpponentInterruption::none;
    if (fake.opponentSilent && !waitUntil([&] { return fake.opponentWasSilent.load(); }))
        return 50;
    std::this_thread::sleep_for(115ms);
    fake.press = true;
    if (!waitUntil([&] { return fake.edgeAt.load() != 0; }))
        return 51;
    fake.press = false;
    if (!waitUntil([&] { return fake.reportsAfterRelease.load() >= 2; }))
        return 52;
    fake.opponentSilent = false;
    if (!waitUntil([&] { return fake.opponentReportsSinceEdge.load() >= 2; }))
        return 53;

    const auto firstEdge = Clock::time_point{Clock::duration{fake.edgeAt.load()}};
    auto result = consume();
    if (interruption == OpponentInterruption::disconnect) {
        // A detected loss invalidates the batch. The next legitimate tap is
        // accepted only after the reconnected pair rearms at rest.
        if (result.armed || !result.shifts.empty() || !waitUntil([&] { return consume().armed; }))
            return 57;
        fake.edgeAt = 0;
        fake.reportsAfterRelease = 0;
        fake.press = true;
        if (!waitUntil([&] { return fake.edgeAt.load() != 0; }))
            return 58;
        fake.press = false;
        if (!waitUntil([&] { return fake.reportsAfterRelease.load() >= 2; }))
            return 59;
        result = consume();
    } else if (!result.armed || result.priorOpposite == Clock::time_point{} ||
               firstEdge - result.priorOpposite <= 100ms) {
        return 54;
    }
    if (interruption == OpponentInterruption::noReport)
        return result.shifts.empty() ? 0 : 55;
    const auto edge = Clock::time_point{Clock::duration{fake.edgeAt.load()}};
    const std::array expected{std::pair{pressDown ? -1 : 1, edge}};
    return result.armed && std::ranges::equal(result.shifts, expected) && consume().shifts.empty() ? 0 : 56;
}

int checkPaddleBatchOrdering()
{
    const auto first = Clock::now();
    PaddleInputBatch batch;
    const auto add = [&](unsigned int down, int shift, auto at) {
        batch.observe({.sequentialArmed = true, .paddlesPressed = down, .shiftDelta = shift,
            .shiftSampledAt = at, .oppositePaddleSampledAt = at}, 100ms);
    };
    add(1, 1, first);
    add(0, 0, first + 5ms);
    add(1, 1, first + 10ms);
    add(2, 0, first + 15ms);
    add(1, 0, first + 20ms);
    add(0, 0, first + 25ms);
    add(1, -1, first + 30ms);
    batch.finish();
    const std::array expected{std::pair{1, first}, std::pair{-1, first + 30ms}};
    if (batch.overflowed() || !std::ranges::equal(batch.edges(), expected))
        return 45;

    PaddleInputBatch full;
    for (int n = 0; n < 5; ++n) {
        const auto at = first + n * 10ms;
        full.observe({.sequentialArmed = true, .paddlesPressed = 1, .shiftDelta = 1,
            .shiftSampledAt = at, .oppositePaddleSampledAt = at}, 100ms);
        full.observe({.sequentialArmed = true}, 100ms);
    }
    return full.overflowed() && full.edges().size() == 4 ? 0 : 46;
}

int checkPaddleEvidenceAtPress()
{
    const auto first = Clock::now();
    PaddleInputBatch batch;
    const auto tap = [&](int shift, auto at, auto opposite) {
        batch.observe({.sequentialArmed = true, .paddlesPressed = 1, .shiftDelta = shift,
                       .shiftSampledAt = at, .oppositePaddleSampledAt = opposite}, 100ms);
        // A later neutral report finishes the gesture. It cannot validate a
        // press that was discarded for lack of evidence.
        batch.observe({.sequentialArmed = true, .sequentialSampledAt = at + 5ms}, 100ms);
    };
    tap(1, first, first - 100ms); // Inclusive age limit.
    tap(1, first + 10ms, first - 91ms);
    tap(-1, first + 20ms, first + 21ms);
    tap(1, first + 30ms, Clock::time_point{});
    tap(-1, first + 40ms, first + 40ms);
    batch.finish();
    const std::array expected{std::pair{1, first}, std::pair{-1, first + 40ms}};
    return !batch.overflowed() && std::ranges::equal(batch.edges(), expected) ? 0 : 47;
}

} // namespace

int main()
{
    for (const auto result : {checkIsolatedDiscovery(false), checkIsolatedDiscovery(true), checkBlockedWorkerTelemetry(),
                              checkCoalescedArming(),
                              checkBufferedTransitions(false), checkBufferedTransitions(true),
                              checkPaddleBatchOrdering(), checkPaddleEvidenceAtPress(),
                              checkSplitPaddleSampling(false, OpponentInterruption::none),
                              checkSplitPaddleSampling(true, OpponentInterruption::none),
                              checkSplitPaddleSampling(false, OpponentInterruption::noReport),
                              checkSplitPaddleSampling(true, OpponentInterruption::noReport),
                              checkSplitPaddleSampling(false, OpponentInterruption::disconnect)}) {
        if (result) {
            std::cerr << "Driving input check failed: " << result << '\n';
            return result;
        }
    }
}
