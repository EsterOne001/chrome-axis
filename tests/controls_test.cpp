#include "chrome_axis/controls.hpp"

#include <cmath>
#include <fstream>
#include <iostream>

namespace {

bool approximatelyEqual(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0001f;
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
        || loaded->steering.deviceId != wheelId || loaded->throttle.positive != 0)
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
    state = reader.update(connected);
    if (state.throttleReady || state.values.throttle != 0.0f || state.steeringReady
        || reader.axisStatuses()[1] != AxisStatus::waitingForNeutral
        || reader.axisStatuses()[2] != AxisStatus::axisUnavailable)
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
    return 0;
}

} // namespace

int main()
{
    const int result = run();
    if (result != 0)
        std::cerr << "Calibration or reconnect regression, check " << result << '\n';
    return result;
}
