#pragma once

#include "chrome_axis/calibration.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>

namespace chrome_axis {

inline constexpr auto guidedCalibrationTimeout = std::chrono::seconds(3);

struct CaptureDeviceRead {
    std::optional<DeviceSample> sample;
    DeviceReadStatus status{DeviceReadStatus::unavailable};
    bool updated{true};

    bool needsRest() const noexcept {
        return status == DeviceReadStatus::unavailable || status == DeviceReadStatus::resumed;
    }
};

inline bool restartAnalogCapture(const CaptureDeviceRead& reading, bool measuringRest, bool hadSignal) noexcept
{
    return reading.needsRest() && (!measuringRest || hadSignal);
}

inline bool calibrationReviewHasReadings(const std::array<bool, 3>& scope,
                                         const std::array<std::optional<float>, 3>& travel) noexcept
{
    for (std::size_t axis = 0; axis < scope.size(); ++axis) {
        if (scope[axis] && !travel[axis])
            return false;
    }
    return true;
}

inline bool collectGuidedCalibrationReading(CalibrationCapture& capture, const CaptureDeviceRead& reading,
                                            bool& hadSignal)
{
    if (!reading.updated)
        return true;
    if (hadSignal && reading.needsRest())
        return false;
    if (!reading.sample) {
        capture = {};
        return true;
    }
    // The inventory may already know the device while the reading handle is
    // still discovering its axes. Wait before collecting the first position.
    const auto& sample = *reading.sample;
    if (!hadSignal && sample.axesPending.any() &&
        std::ranges::none_of(sample.axesPresent, [](bool present) { return present; }))
        return true;
    if (!capture.add(sample))
        return false;
    hadSignal = true;
    return true;
}

} // namespace chrome_axis
