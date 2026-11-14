#include "chrome_axis/controls.hpp"

#include <cmath>
#include <fstream>
#include <iostream>

namespace {

bool approximatelyEqual(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0001f;
}

int testTransientRead(const chrome_axis::Profile& profile)
{
    using namespace chrome_axis;
    using namespace std::chrono_literals;
    using Status = DeviceReadStatus;
    DeviceReadContinuity continuity;
    const auto first = DeviceReadContinuity::Clock::time_point{} + 1s;
    if (continuity.pending(first) != Status::unavailable ||
        continuity.available(first + 1ms) != Status::resumed ||
        continuity.pending(first + 95ms) != Status::pending ||
        continuity.available(first + 100ms) != Status::available ||
        continuity.pending(first + 125ms) != Status::pending ||
        continuity.available(first + 200ms) != Status::resumed ||
        continuity.pending(first + 300ms) != Status::unavailable)
        return 35;
    continuity.reset();
    if (continuity.pending(first + 400ms) != Status::unavailable)
        return 36;

    DeviceSample wheel, pedals;
    wheel.axesPresent[0] = true;
    wheel.axes[0] = profile.steering.neutral;
    pedals.axesPresent[1] = pedals.axesPresent[2] = true;
    pedals.axes[1] = profile.throttle.neutral;
    pedals.axes[2] = profile.brake.neutral;
    ControlReader reader(profile);
    const std::array<const DeviceSample*, 3> connected{&wheel, &pedals, &pedals};
    for (int i = 0; i < 3; ++i)
        reader.update(connected);

    pedals.axes[1] = profile.throttle.positive;
    pedals.axes[2] = profile.brake.positive;
    wheel.axes[0] = profile.steering.positive;
    if (!reader.update(connected).brakeReady)
        return 37;

    ControlReader::Readings readings;
    const std::array pending{Status::available, Status::pending, Status::pending};
    auto state = reader.update({&wheel, nullptr, nullptr}, &readings, pending);
    if (!state.steeringReady || state.throttleReady || state.brakeReady || state.values.brake != 0.0f ||
        readings[1] || readings[2] || reader.axisStatuses()[2] != AxisStatus::temporarilyUnavailable)
        return 38;
    state = reader.update(connected);
    if (!state.throttleReady || !state.brakeReady || state.values.brake != 1.0f)
        return 39;

    reader.update({&wheel, nullptr, nullptr}, nullptr, pending);
    state = reader.update(connected, nullptr, {Status::available, Status::resumed, Status::resumed});
    if (!state.steeringReady || state.throttleReady || state.brakeReady || state.values.brake != 0.0f ||
        reader.axisStatuses()[2] != AxisStatus::waitingForNeutral)
        return 40;
    return 0;
}

int run()
{
    const std::string wheelId = "{11111111-1111-1111-1111-111111111111}";
    const std::string pedalId = "{22222222-2222-2222-2222-222222222222}";
    const chrome_axis::Profile expected{
        {wheelId, 0, 400, 800, 0, 0.02f},
        {pedalId, 1, 1000, 1000, 0, 0.03f},
        {pedalId, 2, 0, 0, 1000, 0.03f}
    };

    const auto path = std::filesystem::current_path()
        / (L"controls-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    if (!chrome_axis::saveProfile(path, expected, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto loaded = chrome_axis::loadProfile(path, error);
    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "version=1\nversion=1\n";
    }
    const bool rejectedDuplicate = !chrome_axis::loadProfile(path, error);
    std::filesystem::remove(path);
    if (!loaded || !rejectedDuplicate || loaded->throttle.deviceId != pedalId
        || loaded->steering.deviceId != wheelId || loaded->throttle.positive != 0
        || loaded->brakeResponse != chrome_axis::BrakeResponse{})
        return 2;

    using chrome_axis::AxisStatus;
    chrome_axis::DeviceSample wheel;
    wheel.axesPresent[0] = true;
    wheel.axes[0] = 400;
    chrome_axis::DeviceSample pedals;
    pedals.axesPresent[1] = pedals.axesPresent[2] = true;
    pedals.axes[1] = 1000;
    chrome_axis::ControlReader reader(*loaded);
    const std::array<const chrome_axis::DeviceSample*, 3> connected{&wheel, &pedals, &pedals};
    chrome_axis::ControlState state;
    for (int i = 0; i < 3; ++i)
        state = reader.update(connected);
    if (!state.steeringReady || !state.throttleReady || !state.brakeReady
        || reader.axisStatuses() != std::array{AxisStatus::armed, AxisStatus::armed, AxisStatus::armed}
        || state.values.steering != 0.0f || state.values.throttle != 0.0f || state.values.brake != 0.0f)
        return 3;

    wheel.axes[0] = 0;
    pedals.axes[1] = 500;
    pedals.axes[2] = 1000;
    state = reader.update(connected);
    if (!approximatelyEqual(state.values.steering, 1.0f)
        || !approximatelyEqual(state.values.throttle, 0.47f / 0.97f)
        || !approximatelyEqual(state.values.brake, 1.0f))
        return 4;

    wheel.axes[0] = 800;
    state = reader.update({&wheel, nullptr, nullptr});
    if (!approximatelyEqual(state.values.steering, -1.0f) || state.throttleReady || state.brakeReady
        || reader.axisStatuses()[1] != AxisStatus::deviceUnavailable
        || state.values.throttle != 0.0f || state.values.brake != 0.0f)
        return 5;
    for (int i = 0; i < 4; ++i)
        state = reader.update(connected);
    if (state.throttleReady || state.brakeReady)
        return 6;

    pedals.axes[1] = 1000;
    pedals.axes[2] = 0;
    for (int i = 0; i < 3; ++i)
        state = reader.update(connected);
    if (!state.throttleReady || !state.brakeReady)
        return 7;
    reader.reset();
    pedals.axes[1] = 0;
    pedals.axesPresent[2] = false;
    chrome_axis::ControlReader::Readings readings;
    state = reader.update(connected, &readings);
    if (state.throttleReady || state.values.throttle != 0.0f || state.steeringReady
        || reader.axisStatuses()[1] != AxisStatus::waitingForNeutral
        || reader.axisStatuses()[2] != AxisStatus::axisUnavailable || readings[1] != 1.0f || readings[2])
        return 8;
    pedals.axesPresent[2] = true;

    auto invalid = *loaded;
    invalid.brake.axis = invalid.throttle.axis;
    if (chrome_axis::validateProfile(invalid, error))
        return 9;

    auto combined = *loaded;
    combined.throttle = {pedalId, 1, 500, 500, 1000, 0.03f};
    combined.brake = {pedalId, 1, 507, 507, 0, 0.03f};
    const auto savedCombined = chrome_axis::saveProfile(path, combined, error);
    const auto loadedCombined = savedCombined ? chrome_axis::loadProfile(path, error) : std::nullopt;
    std::filesystem::remove(path);
    if (!loadedCombined || loadedCombined->brake.axis != loadedCombined->throttle.axis)
        return 10;

    chrome_axis::ControlReader combinedReader(*loadedCombined);
    pedals.axes[1] = combined.brake.neutral;
    for (int i = 0; i < 3; ++i)
        state = combinedReader.update(connected);
    if (!state.throttleReady || !state.brakeReady || state.values.throttle != 0.0f || state.values.brake != 0.0f)
        return 11;

    pedals.axes[1] = combined.throttle.positive;
    state = combinedReader.update(connected);
    if (!approximatelyEqual(state.values.throttle, 1.0f) || state.values.brake != 0.0f)
        return 12;

    pedals.axes[1] = combined.brake.positive;
    state = combinedReader.update(connected);
    if (!approximatelyEqual(state.values.brake, 1.0f) || state.values.throttle != 0.0f)
        return 13;

    combinedReader.update({&wheel, nullptr, nullptr});
    for (int i = 0; i < 3; ++i)
        state = combinedReader.update(connected);
    if (state.throttleReady || state.brakeReady || state.values.throttle != 0.0f || state.values.brake != 0.0f)
        return 14;

    pedals.axes[1] = combined.throttle.neutral;
    for (int i = 0; i < 3; ++i)
        state = combinedReader.update(connected);
    if (!state.throttleReady || !state.brakeReady)
        return 15;

    invalid = combined;
    invalid.brake.positive = combined.throttle.positive;
    if (chrome_axis::validateProfile(invalid, error))
        return 16;

    invalid = combined;
    invalid.brake.neutral = combined.throttle.neutral + 50;
    if (chrome_axis::validateProfile(invalid, error))
        return 17;

    invalid = combined;
    invalid.steering.deviceId = pedalId;
    invalid.steering.axis = combined.throttle.axis;
    if (chrome_axis::validateProfile(invalid, error))
        return 18;

    const chrome_axis::Profile steeringOnly{loaded->steering, {}, {}};
    const auto savedPartial = chrome_axis::saveProfile(path, steeringOnly, error);
    const auto loadedPartial = savedPartial ? chrome_axis::loadProfile(path, error) : std::nullopt;
    {
        std::ofstream broken(path, std::ios::app);
        broken << "throttle.axis=1\n";
    }
    const bool rejectedIncompleteChannel = !chrome_axis::loadProfile(path, error);
    std::filesystem::remove(path);
    if (!loadedPartial || !rejectedIncompleteChannel || !loadedPartial->throttle.deviceId.empty()
        || !loadedPartial->brake.deviceId.empty() || chrome_axis::validateProfile({}, error))
        return 19;

    chrome_axis::ControlReader steeringReader(*loadedPartial);
    wheel.axes[0] = loadedPartial->steering.neutral;
    for (int i = 0; i < 3; ++i)
        state = steeringReader.update(connected);
    if (!state.steeringReady || state.throttleReady || state.brakeReady
        || steeringReader.axisStatuses() != std::array{AxisStatus::armed, AxisStatus::unassigned, AxisStatus::unassigned})
        return 20;

    // Both polarities can drift past the captured rest point without producing pedal input.
    reader.reset();
    pedals.axes[1] = 1040;
    pedals.axes[2] = -40;
    for (int i = 0; i < 3; ++i)
        state = reader.update(connected);
    if (!state.throttleReady || !state.brakeReady || state.values.throttle != 0.0f || state.values.brake != 0.0f)
        return 21;

    pedals.axes[1] = 500;
    pedals.axes[2] = 500;
    state = reader.update(connected);
    if (state.values.throttle <= 0.0f || state.values.brake <= 0.0f)
        return 22;

    // Preview reports the calibrated physical travel and the value sent after shaping.
    using chrome_axis::previewBrake;
    auto tuned = *loaded;
    tuned.brake.deadzone = 0.10f;
    tuned.brakeResponse = {100, 70};
    const auto midpoint = previewBrake(tuned.brake, 400, tuned.brakeResponse);
    if (!approximatelyEqual(midpoint.raw, 0.4f) || !approximatelyEqual(midpoint.adjusted, 0.25f)
        || !approximatelyEqual(previewBrake(tuned.brake, 400, {-100, 70}).adjusted, 0.75f)
        || !approximatelyEqual(previewBrake(tuned.brake, 400, {0, 70}).adjusted, 0.5f)
        || previewBrake(tuned.brake, 100, tuned.brakeResponse).adjusted != 0.0f
        || previewBrake(tuned.brake, -100, tuned.brakeResponse).raw != 0.0f
        || previewBrake(tuned.brake, 699, tuned.brakeResponse).adjusted >= 1.0f
        || previewBrake(tuned.brake, 700, tuned.brakeResponse).adjusted != 1.0f
        || previewBrake(tuned.brake, 1100, tuned.brakeResponse).raw != 1.0f
        || !approximatelyEqual(previewBrake(loaded->brake, 500, {}).adjusted, 0.47f / 0.97f))
        return 23;

    const auto savedTuned = chrome_axis::saveProfile(path, tuned, error);
    const auto loadedTuned = savedTuned ? chrome_axis::loadProfile(path, error) : std::nullopt;
    auto invalidResponse = tuned;
    invalidResponse.brakeResponse.saturation = 49;
    const auto rejectedInvalidSave = !chrome_axis::saveProfile(path, invalidResponse, error);
    const auto unchangedTuned = chrome_axis::loadProfile(path, error);
    const auto savedLegacy = chrome_axis::saveProfile(path, *loaded, error);
    {
        std::ofstream malformed(path, std::ios::app);
        malformed << "brake_response.progressivity=101\n";
    }
    const auto rejectedInvalidRead = !chrome_axis::loadProfile(path, error);
    std::filesystem::remove(path);
    if (!loadedTuned || loadedTuned->brakeResponse != tuned.brakeResponse || !rejectedInvalidSave
        || !unchangedTuned || unchangedTuned->brakeResponse != tuned.brakeResponse || !savedLegacy || !rejectedInvalidRead)
        return 24;

    chrome_axis::ControlReader tunedReader(*loadedTuned);
    chrome_axis::ControlReader::CalibrationReadings review;
    wheel.axes[0] = 500;
    pedals.axes[1] = 750;
    pedals.axes[2] = 400;
    for (int i = 0; i < 4; ++i)
        state = tunedReader.update(connected, &readings, {}, &review);
    if (state.brakeReady || state.values.brake != 0.0f || !readings[2]
        || !approximatelyEqual(*readings[2], 0.3f / 0.9f))
        return 25;
    if (review.travel != std::array<std::optional<float>, 3>{-0.25f, 0.25f, midpoint.raw}
        || !review.adjustedBrake || !approximatelyEqual(*review.adjustedBrake, midpoint.adjusted))
        return 41;
    tunedReader.update({&wheel, nullptr, nullptr}, nullptr, {}, &review);
    if (!review.travel[0] || review.travel[1] || review.travel[2] || review.adjustedBrake)
        return 42;

    pedals.axes[2] = 0;
    for (int i = 0; i < 3; ++i)
        state = tunedReader.update(connected);
    pedals.axes[2] = 400;
    state = tunedReader.update(connected, &readings);
    if (!state.brakeReady || !approximatelyEqual(state.values.brake, midpoint.adjusted)
        || !readings[2] || !approximatelyEqual(*readings[2], 0.3f / 0.9f))
        return 26;

    combined.brakeResponse = {100, 70};
    chrome_axis::ControlReader tunedCombinedReader(combined);
    pedals.axes[1] = combined.brake.neutral;
    for (int i = 0; i < 3; ++i)
        state = tunedCombinedReader.update(connected);
    pedals.axes[1] = combined.throttle.positive;
    state = tunedCombinedReader.update(connected);
    if (!state.throttleReady || !state.brakeReady || state.values.throttle != 1.0f || state.values.brake != 0.0f)
        return 27;
    pedals.axes[1] = 250;
    state = tunedCombinedReader.update(connected);
    if (state.values.throttle != 0.0f || !approximatelyEqual(state.values.brake,
            previewBrake(combined.brake, 250, combined.brakeResponse).adjusted))
        return 28;

    auto zeroSteering = *loaded;
    zeroSteering.steering.deadzone = 0.0f;
    const auto savedZero = chrome_axis::saveProfile(path, zeroSteering, error);
    const auto loadedZero = savedZero ? chrome_axis::loadProfile(path, error) : std::nullopt;
    std::filesystem::remove(path);
    if (!loadedZero || loadedZero->steering.deadzone != 0.0f)
        return 29;

    chrome_axis::ControlReader zeroSteeringReader(*loadedZero);
    wheel.axes[0] = 390;
    for (int i = 0; i < 3; ++i)
        state = zeroSteeringReader.update(connected);
    if (state.steeringReady || state.values.steering != 0.0f)
        return 30;
    for (const auto noisyCenter : {402L, 398L, 401L}) {
        wheel.axes[0] = noisyCenter;
        state = zeroSteeringReader.update(connected);
    }
    if (!state.steeringReady || !approximatelyEqual(state.values.steering, -0.0025f))
        return 31;
    wheel.axes[0] = 399;
    state = zeroSteeringReader.update(connected);
    if (!state.steeringReady || !approximatelyEqual(state.values.steering, 0.0025f))
        return 32;

    invalid = zeroSteering;
    invalid.brake.deadzone = 0.0f;
    if (chrome_axis::validateProfile(invalid, error))
        return 33;
    zeroSteering.steering.deadzone = 0.001f;
    if (!chrome_axis::validateProfile(zeroSteering, error))
        return 34;
    return testTransientRead(*loaded);
}

} // namespace

int main()
{
    const int result = run();
    if (result != 0)
        std::cerr << "Calibration or reconnect regression, check " << result << '\n';
    return result;
}
