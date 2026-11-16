#define DIRECTINPUT_VERSION 0x0800

#include "chrome_axis/feedback.hpp"
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"
#include "../src/core/device_object_probe.hpp"
#include "../src/core/device_layout_worker.hpp"
#include "../src/core/device_read_recovery.hpp"
#include "../src/core/wheel_force_retry.hpp"
#include "../src/plugin/force_output_worker.hpp"

#include <windows.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <tuple>

namespace {

using namespace std::chrono_literals;
using TestClock = std::chrono::steady_clock;
using namespace chrome_axis;

struct TestForceContext {
    TestClock::time_point expires;
    bool current() const noexcept { return TestClock::now() < expires; }
    TestClock::time_point deadline() const noexcept { return expires; }
};

struct FakeForceDriver {
    struct State {
        std::atomic<bool> entered{};
        std::atomic<bool> resume{};
        std::atomic<unsigned int> initialized{};
        std::atomic<unsigned int> setupCompleted{};
        std::atomic<unsigned int> stops{};
        std::atomic<unsigned int> started{};
        std::atomic<std::uint64_t> recoveryToken{};
    };
    static inline State* testState{};

    FakeForceDriver(HWND, DirectInputDiagnostic, DirectInputReadObserver observer, void* context) noexcept
        : observer(observer), observerContext(context) {}

    bool ready() const noexcept { return true; }
    WheelForceState wheelForceState() const noexcept { return mode; }

    std::optional<float> setWheelForce(std::string_view id, std::size_t, float force,
                                       WheelForceGuard guard) {
        if (!initialized) {
            if (++testState->initialized == 1) {
                const auto start = TestClock::now();
                testState->entered.store(true);
                testState->resume.wait(false);
                const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(TestClock::now() - start);
                observer(observerContext, {DirectInputReadEventKind::slow, id, "FFB Acquire", S_OK,
                    static_cast<std::uint64_t>(duration.count())});
            }
            if (!guard.canInitialize(guard.context))
                return std::nullopt;
            initialized = true;
            ++testState->setupCompleted;
        }
        if (!guard.current(guard.context))
            return std::nullopt;
        ++testState->started;
        testState->recoveryToken.store(guard.recoveryToken);
        mode = WheelForceState::active;
        return force;
    }

    void stopWheelForce() noexcept {
        ++testState->stops;
        initialized = false;
        mode = WheelForceState::waiting;
    }

    DirectInputReadObserver observer{};
    void* observerContext{};
    bool initialized{};
    WheelForceState mode{WheelForceState::waiting};
};

template <class Predicate>
bool waitFor(Predicate&& predicate)
{
    const auto deadline = TestClock::now() + 2s;
    while (!predicate()) {
        if (TestClock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

int testSlowForceDuringIndependentInput(bool cancel, bool recover = false)
{
    FakeForceDriver::State state;
    FakeForceDriver::testState = &state;
    ForceOutputWorker<TestForceContext, FakeForceDriver> worker;
    const auto allowDriver = [&] {
        state.resume.store(true);
        state.resume.notify_one();
    };
    struct AllowDriverOnExit {
        const decltype(allowDriver)& allow;
        ~AllowDriverOnExit() { allow(); }
    } restore{allowDriver};

    const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
    const std::string pedal = "{22222222-2222-2222-2222-222222222222}";
    const HWND window = GetDesktopWindow();
    auto now = TestClock::now();
    if (!worker.submit(window, wheel, 0, 0.75f, {now + 100ms}, 42) ||
        !waitFor([&] { return state.entered.load(); }))
        return 51;

    Profile profile;
    profile.brake = {pedal, 0, 0, 0, 1000, 0.02f};
    ControlReader controls(profile);
    AccessoryReader accessories({{pedal, AccessoryKind::Button, 0, 0, 1, VK_SPACE}});
    DeviceSample input;
    input.axesPresent[0] = true;
    input.buttonsPresent.set(0);
    std::array<const DeviceSample*, 3> axisSamples{nullptr, nullptr, &input};
    std::array<const DeviceSample*, 1> buttonSamples{&input};
    for (int i = 0; i < 5; ++i) {
        input.axes[0] = 0;
        input.buttons.reset(0);
        for (int neutral = 0; neutral < 3; ++neutral) {
            controls.update(axisSamples);
            accessories.update(buttonSamples);
        }
        input.axes[0] = 1000;
        input.buttons.set(0);
        if (controls.update(axisSamples).values.brake != 1.0f || !accessories.update(buttonSamples).test(VK_SPACE))
            return 52;
        std::this_thread::sleep_for(25ms);
    }

    if (cancel)
        worker.cancel();
    const auto latestDeadline = TestClock::now() + (cancel ? 1s : 350ms);
    const std::uint64_t recoveryToken = recover ? 17 : 0;
    if (!worker.submit(window, wheel, 0, 0.25f, {latestDeadline}, 42, false, recoveryToken) ||
        !worker.submit(window, wheel, 0, 0.5f, {latestDeadline}, 42, false, recoveryToken))
        return 53;
    allowDriver();
    std::optional<decltype(worker)::Completion> accepted;
    if (!waitFor([&] {
            while (auto result = worker.nextCompletion()) {
                if (result->accepted)
                    accepted = result;
            }
            return accepted.has_value();
        }) || accepted->accepted != 0.5f || accepted->captureId != 42 || state.started.load() != 1 ||
        state.initialized.load() != (cancel || recover ? 2u : 1u) ||
        state.setupCompleted.load() != 1 || state.recoveryToken.load() != recoveryToken)
        return 54;
    const auto slow = worker.nextOperation();
    if (!slow || slow->name.data() != std::string_view("FFB Acquire") || slow->deviceId.data() != wheel ||
        slow->durationMicros <= 100000)
        return 55;
    const auto previousStops = state.stops.load();
    if (cancel)
        worker.cancel();
    if (!waitFor([&] { return state.stops.load() > previousStops; }))
        return 56;
    return 0;
}

int testWheelForceRecoveryBackoff()
{
    WheelForceRetry retry;
    const auto started = TestClock::time_point{} + 100s;
    for (const auto elapsed : {0s, 2s, 6s, 14s, 30s}) {
        const auto now = started + elapsed;
        if (retry.waiting(now))
            return 62;
        retry.failed(now);
    }
    const auto recoveredAt = started + 31s;
    if (retry.recovered(0) || !retry.waiting(recoveredAt) || retry.waiting(started + 60s))
        return 63;
    if (!retry.recovered(17) || retry.waiting(recoveredAt))
        return 64;
    retry.failed(recoveredAt);
    return retry.recovered(17) || retry.recovered(0) || !retry.waiting(recoveredAt + 1s) ||
        retry.waiting(recoveredAt + 2s) ? 65 : 0;
}

int testWheelForceRecoversFromTemporaryAccessDenial()
{
    WheelForceRetry retry;
    const auto start = TestClock::time_point{} + 100s;
    for (int i = 0; i <= 15; ++i) {
        const auto now = start + std::chrono::seconds(2 * i);
        if (retry.waiting(now))
            return 72;
        retry.failed(now, true);
        if (!retry.waiting(now + 1999ms) || retry.waiting(now + 2s))
            return 73;
    }
    return retry.waiting(start + 32s) ? 74 : 0;
}

int testWheelForceRecoveryAfterPendingReopen()
{
    DeviceReadRecovery input;
    DeviceReadRecovery otherDevice;
    WheelForceRetry force;
    std::uint64_t token{};
    const auto acceptReport = [&] {
        if (input.accepted())
            ++token;
        return force.recovered(token);
    };
    const auto start = TestClock::time_point{} + 100s;
    for (const auto elapsed : {0s, 2s, 6s, 14s, 30s})
        force.failed(start + elapsed);

    // A short interruption reuses the open handle and keeps the force retry deadline.
    const auto now = start + 31s;
    const auto transient = input.pending(now);
    const auto beforeReopen = input.pending(now + 499ms);
    if (!transient.first || transient.reopen || beforeReopen.first || beforeReopen.reopen ||
        acceptReport() || !force.waiting(now + 499ms))
        return 66;

    // A forced reopen still needs a successful report, even after additional failures.
    const auto stalled = now + 1s;
    if (!input.pending(stalled).first || !input.pending(stalled + 500ms).reopen)
        return 67;
    input.failed(false);
    input.failed(false);
    if (otherDevice.accepted() || force.recovered(token) || !force.waiting(stalled + 1500ms))
        return 68;
    const auto recovered = stalled + 1500ms;
    if (!acceptReport() || !token || force.waiting(recovered))
        return 69;

    // Duplicate successes and context pauses can leave the same token in the report.
    force.failed(recovered);
    if (acceptReport() || !force.waiting(recovered + 1s) || force.waiting(recovered + 2s))
        return 70;
    input.failed(true);
    input.failed(false);
    return acceptReport() && !force.waiting(recovered + 1s) && !acceptReport() ? 0 : 71;
}

int testSlowLayoutRetryKeepsKnownAxis()
{
    DeviceObjectSelection wanted;
    wanted.axes.set(0);
    wanted.buttons.set(0);
    std::atomic<unsigned int> attempts{};
    std::atomic<bool> busy{};
    DeviceLayoutWorker worker([&](const DeviceLayoutRequest& request) {
        DeviceLayoutSnapshot result;
        if (request.connection == 1) {
            result.present.axes = request.wanted.axes;
            result.pending.buttons = request.wanted.buttons;
            if (++attempts > 1) {
                busy = true;
                std::this_thread::sleep_for(150ms);
                busy = false;
            }
        } else {
            result.present.buttons = request.wanted.buttons;
        }
        return result;
    });
    const std::string device = "{11111111-1111-1111-1111-111111111111}";
    if (!waitFor([&] { return worker.read(device, wanted, 1).present.axes.test(0); }))
        return 75;
    if (!waitFor([&] {
        const auto layout = worker.read(device, wanted, 1);
        return busy && layout.present.axes.test(0) && layout.pending.buttons.test(0);
    }))
        return 76;
    // A result still being queried on the old connection must not be published after reopen.
    const auto reopened = worker.read(device, wanted, 2);
    if (reopened.present.axes.any() || reopened.pending != wanted)
        return 77;
    return waitFor([&] {
        const auto layout = worker.read(device, wanted, 2);
        return !layout.incomplete() && !layout.present.axes.any() && layout.present.buttons.test(0);
    }) ? 0 : 78;
}

int testProvisionalInputLayout()
{
    DeviceObjectProbe<3> layout;
    const auto now = TestClock::now();
    std::array<unsigned int, 3> attempts{};
    const auto probe = [&](std::size_t index) -> std::tuple<HRESULT, bool, TestClock::time_point> {
        ++attempts[index];
        if (index == 0 && attempts[index] == 1)
            return {E_PENDING, false, now + 10ms};
        if (index == 2)
            return {DIERR_OBJECTNOTFOUND, false, now};
        return {S_OK, true, now};
    };
    const auto all = std::bitset<3>{}.set();
    layout.refresh(all, now, probe);
    if (layout.pending().to_ulong() != 0b001 || layout.present().to_ulong() != 0b010)
        return 57;
    layout.refresh(all, now + 509ms, probe);
    if (attempts != std::array<unsigned int, 3>{1, 1, 1})
        return 58;
    layout.refresh(all, now + 510ms, probe);
    if (layout.pending().any() || layout.present().to_ulong() != 0b011 ||
        attempts != std::array<unsigned int, 3>{2, 1, 1})
        return 59;

    const std::string device = "{33333333-3333-3333-3333-333333333333}";
    Profile profile;
    profile.brake = {device, 2, 0, 0, 1000, 0.02f};
    ControlReader controls(profile);
    AccessoryReader accessories({{device, AccessoryKind::Button, 0, 0, 1, VK_SPACE},
                                  {device, AccessoryKind::Pov, 1, povCentered, 0, VK_LEFT}});
    DeviceSample input;
    input.axesPending.set(2);
    input.buttonsPending.set(0);
    input.povsPending.set(1);
    std::array<const DeviceSample*, 3> axisSamples{nullptr, nullptr, &input};
    std::array<const DeviceSample*, 2> accessorySamples{&input, &input};
    std::array<AccessoryObservation, 2> observations;
    controls.update(axisSamples);
    accessories.update(accessorySamples, {}, observations);
    if (controls.axisStatuses()[2] != AxisStatus::temporarilyUnavailable ||
        observations[0].status != AccessoryStatus::temporarilyUnavailable ||
        observations[1].status != AccessoryStatus::temporarilyUnavailable)
        return 60;
    input.axesPending.reset();
    input.buttonsPending.reset();
    input.povsPending.reset();
    controls.update(axisSamples);
    accessories.update(accessorySamples, {}, observations);
    return controls.axisStatuses()[2] == AxisStatus::axisUnavailable &&
        observations[0].status == AccessoryStatus::inputUnavailable &&
        observations[1].status == AccessoryStatus::inputUnavailable ? 0 : 61;
}

} // namespace

int main()
{
    using namespace chrome_axis;
    if (const auto result = testWheelForceRecoveryBackoff())
        return result;
    if (const auto result = testWheelForceRecoversFromTemporaryAccessDenial())
        return result;
    if (const auto result = testWheelForceRecoveryAfterPendingReopen())
        return result;
    if (const auto result = testSlowLayoutRetryKeepsKnownAxis())
        return result;
    if (const auto result = testProvisionalInputLayout())
        return result;
    if (const auto result = testSlowForceDuringIndependentInput(false))
        return result;
    if (const auto result = testSlowForceDuringIndependentInput(true))
        return result;
    if (const auto result = testSlowForceDuringIndependentInput(false, true))
        return result;
    FeedbackSettings settings;
    WheelFeedback feedback;
    FeedbackSample motion{0.8f, 20.0f, 0.0f, 1600.0f};
    const auto disabled = feedback.update(settings, motion, 0.02f);
    if (disabled.force != 0.0f || disabled.requestedForce != 0.0f || disabled.saturation != FeedbackSaturation::none)
        return 1;
    settings.enabled = true;
    settings.strength = 35;

    const auto settleOutput = [&](const FeedbackSample& sample) {
        feedback.reset();
        FeedbackOutput value{};
        for (int i = 0; i < 30; ++i) {
            value = feedback.update(settings, sample, 0.02f);
        }
        return value;
    };
    const auto settle = [&](const FeedbackSample& sample) { return settleOutput(sample).force; };
    const float legacyMoving = settle(motion);
    settings.strength = 100;
    const float moving = settle(motion);
    if (!(moving < legacyMoving) || std::abs(legacyMoving - moving * 0.35f) > 0.0001f)
        return 10;
    settings.strength = 200;
    if (std::abs(settle(motion) - moving * 2.0f) > 0.0001f)
        return 13;
    settings.strength = 100;
    motion.forwardSpeed = 0.0f;
    const float stopped = settle(motion);
    if (!(moving < stopped && stopped < 0.0f))
        return 2;
    motion.forwardSpeed = 20.0f;
    motion.mass = 800.0f;
    if (!(settle(motion) > moving))
        return 3;

    const FeedbackSample straight{0.30f, 20.0f, 0.0f, 1600.0f};
    auto turn = straight;
    turn.lateralAcceleration = 4.0f;
    turn.yawRate = 0.35f;
    const float straightForce = settle(straight);
    const float turnForce = settle(turn);
    if (!(turnForce < straightForce - 0.02f))
        return 15;
    turn.skidding = true;
    const float firstSkidForce = feedback.update(settings, turn, 0.02f).force;
    const float skidForce = settle(turn);
    if (!(turnForce < firstSkidForce && firstSkidForce < skidForce && skidForce < 0.0f))
        return 16;
    turn.skidding = false;
    const float firstRecovered = feedback.update(settings, turn, 0.02f).force;
    for (int i = 0; i < 15; ++i)
        feedback.update(settings, turn, 0.02f);
    const float laterRecovered = feedback.update(settings, turn, 0.02f).force;
    if (!(turnForce < laterRecovered && laterRecovered < firstRecovered && firstRecovered < skidForce) ||
        !(skidForce - firstRecovered < firstSkidForce - turnForce))
        return 30;

    // Lateral motion alone gives a graded reduction. The game skid flag adds to that reduction.
    auto drifting = turn;
    drifting.sideSpeed = 5.0f;
    const float mildSlipForce = settle(drifting);
    drifting.sideSpeed = 14.0f;
    const float heavySlipForce = settle(drifting);
    drifting.skidding = true;
    const float heavySkidForce = settle(drifting);
    if (!(turnForce < mildSlipForce && mildSlipForce < heavySlipForce && heavySlipForce < heavySkidForce &&
          heavySkidForce < 0.0f))
        return 31;

    auto invalidTurn = turn;
    invalidTurn.sideSpeed = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = feedback.update(settings, invalidTurn, 0.02f);
    const float freshForce = WheelFeedback{}.update(settings, turn, 0.02f).force;
    if (invalid.force != 0.0f || invalid.requestedForce != 0.0f || invalid.saturation != FeedbackSaturation::none ||
        feedback.update(settings, turn, 0.02f).force != freshForce)
        return 32;

    settle(turn);
    turn.airborne = true;
    const float firstAirborne = feedback.update(settings, turn, 0.02f).force;
    const float airborneForce = settle(turn);
    if (!(turnForce < firstAirborne && firstAirborne < airborneForce && airborneForce < 0.0f))
        return 17;
    settings.centering = 150;
    const auto stronger = settleOutput(straight);
    if (std::abs(stronger.force - 1.5f * straightForce) > 0.0001f || stronger.saturation != FeedbackSaturation::none)
        return 18;
    settings.centering = 100;
    turn.airborne = false;
    turn.steering = -turn.steering;
    turn.lateralAcceleration = -turn.lateralAcceleration;
    turn.yawRate = -turn.yawRate;
    if (std::abs(settle(turn) + turnForce) > 0.0001f)
        return 19;

    motion.sideSpeed = 0.0f;
    motion.steering = 0.0f;
    motion.impactSpeed = 22.0f;
    motion.impactSide = -0.7f;
    const float impact = settle(motion);
    if (!(impact < -0.35f && impact >= -1.0f))
        return 4;
    settings.impactStrength = 150;
    if (std::abs(settle(motion) - 1.5f * impact) > 0.0001f || settle(straight) != straightForce)
        return 26;
    settings.impactStrength = 0;
    if (settle(motion) != 0.0f || settle(straight) != straightForce)
        return 27;
    settings.impactStrength = 100;
    motion.impactSide = 0.35f;
    if (std::abs(settle(motion) + 0.5f * impact) > 0.0001f)
        return 20;
    motion.impactSide = 0.0f;
    if (settle(motion) != 0.0f)
        return 21;
    motion.impactAge = 0.06f;
    const float frontal = settle(motion);
    motion.impactAge = 0.18f;
    if (!(frontal > 0.0f) || std::abs(settle(motion) + frontal) > 0.0001f)
        return 22;
    motion.impactAge = 0.0f;
    motion.impactSide = -0.7f;
    settings.invert = true;
    if (std::abs(settle(motion) + impact) > 0.0001f)
        return 5;
    settings.impacts = false;
    if (settle(motion) != 0.0f)
        return 6;
    settings.impacts = true;
    motion.impactAge = 0.26f;
    if (settle(motion) != 0.0f)
        return 7;

    FeedbackSample landing;
    landing.landingSpeed = 6.0f;
    landing.landingAge = 0.045f;
    const float landingForce = settle(landing);
    settings.impactStrength = 150;
    if (std::abs(settle(landing) - 1.5f * landingForce) > 0.0001f)
        return 28;
    settings.impactStrength = 0;
    if (settle(landing) != 0.0f)
        return 29;
    settings.impactStrength = 100;
    landing.landingAge = 0.135f;
    if (!(landingForce < 0.0f && landingForce > impact) ||
        std::abs(settle(landing) + landingForce) > 0.0001f)
        return 23;
    landing.airborne = true;
    if (settle(landing) != 0.0f)
        return 24;
    landing.airborne = false;
    landing.impactSpeed = motion.impactSpeed;
    landing.impactSide = motion.impactSide;
    if (std::abs(settle(landing) + impact) > 0.0001f)
        return 25;

    motion.forwardSpeed = std::numeric_limits<float>::infinity();
    if (feedback.update(settings, motion, 0.02f).force != 0.0f)
        return 8;

    // Centering and impact can add up to more than the nominal force.
    const FeedbackSample hardImpact{-1.0f, 120.0f, 0.0f, 3000.0f, 22.0f, 1.0f};
    auto strongCollision = hardImpact;
    strongCollision.steering = -0.85f;
    const auto impactPushedOverLimit = settleOutput(strongCollision);
    if (impactPushedOverLimit.force != -1.0f || impactPushedOverLimit.requestedForce >= -1.0f ||
        impactPushedOverLimit.saturation != FeedbackSaturation::transient)
        return 11;
    settings.strength = 200;
    settings.impactStrength = maxFeedbackImpactPercent;
    const auto both = settleOutput(hardImpact);
    if (both.force != -1.0f || both.requestedForce >= -1.0f || both.saturation != FeedbackSaturation::continuous)
        return 14;
    settings.invert = false;
    const auto reverse = settleOutput(hardImpact);
    if (reverse.force != 1.0f || reverse.requestedForce != -both.requestedForce ||
        reverse.saturation != both.saturation)
        return 12;
    auto opposingImpact = hardImpact;
    opposingImpact.impactSide = -1.0f;
    const auto cancelled = settleOutput(opposingImpact);
    if (cancelled.saturation != FeedbackSaturation::none || cancelled.requestedForce != cancelled.force)
        return 33;
    opposingImpact.steering = -0.75f;
    const auto clippedOpposite = settleOutput(opposingImpact);
    if (clippedOpposite.force != -1.0f || clippedOpposite.saturation != FeedbackSaturation::transient)
        return 34;
    FeedbackSample hardLanding{0.7f, 40.0f, 0.0f, 1700.0f};
    hardLanding.landingSpeed = 9.0f;
    hardLanding.landingAge = 0.135f;
    const auto landingPushedOverLimit = settleOutput(hardLanding);
    if (landingPushedOverLimit.force != -1.0f || landingPushedOverLimit.requestedForce >= -1.0f ||
        landingPushedOverLimit.saturation != FeedbackSaturation::transient)
        return 35;

    settings = {.enabled = true, .strength = 100};
    const FeedbackSample parking{0.5f, 0.0f, 0.0f, 1700.0f};
    const float standardParking = settle(parking);
    settings.lowSpeedEffort = 200;
    if (!(settle(parking) < standardParking - 0.045f))
        return 36;
    settings.lowSpeedEffort = 0;
    if (settle(parking) != 0.0f)
        return 37;
    settings.lowSpeedEffort = 100;
    const auto forceInMotion = [&](int damping, float speed) {
        settings.damping = damping;
        feedback.reset();
        auto sample = parking;
        sample.steering = 0.0f;
        sample.forwardSpeed = speed;
        feedback.update(settings, sample, 0.020f);
        sample.steering = 0.1f;
        return feedback.update(settings, sample, 0.020f).continuousForce;
    };
    if (!(forceInMotion(200, 10.0f) < forceInMotion(0, 10.0f) - 0.02f))
        return 38;

    const auto path = std::filesystem::current_path()
        / (L"feedback-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    const auto defaults = loadFeedbackSettings(path, error);
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=35\nimpacts=0\ninvert=1\n";
    }
    const bool legacy = loadFeedbackSettings(path, error) == FeedbackSettings{true, 35, false, true};
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=35\nimpacts=0\ninvert=1\ncentering=165\n";
    }
    const bool previous = loadFeedbackSettings(path, error) == FeedbackSettings{true, 35, false, true, 165};
    settings.centering = 165;
    settings.impactStrength = 135;
    settings.lowSpeedEffort = 180;
    settings.damping = 45;
    const bool roundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessive{true, 201, true, false};
    const bool rejectedSave = !saveFeedbackSettings(path, excessive, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessiveCentering{true, 200, true, false, 251};
    const bool rejectedCentering = !saveFeedbackSettings(path, excessiveCentering, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessiveImpact{true, 200, true, false, 100, maxFeedbackImpactPercent + 1};
    const bool rejectedImpact = !saveFeedbackSettings(path, excessiveImpact, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    settings.impactStrength = 0;
    const bool muteRoundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\ncentering=49\n";
    }
    const bool rejected = !loadFeedbackSettings(path, error) && !error.empty();
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\nimpact_strength=-1\n";
    }
    const bool rejectedNegativeImpact = !loadFeedbackSettings(path, error) && !error.empty();
    std::filesystem::remove(path);
    return defaults == FeedbackSettings{} && legacy && previous && roundTrip && muteRoundTrip && rejectedSave &&
        rejectedCentering && rejectedImpact && rejected && rejectedNegativeImpact ? 0 : 9;
}
