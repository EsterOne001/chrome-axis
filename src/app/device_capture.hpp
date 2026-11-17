#pragma once

#include "chrome_axis/calibration.hpp"

#include <algorithm>
#include <chrono>

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

inline bool guidedDeviceRecovered(std::optional<std::uint64_t>& lastRecovery, std::uint64_t recovery) noexcept
{
    if (!lastRecovery) {
        lastRecovery = recovery;
        return false;
    }
    // Zero also occurs after reopening the reader on focus change. Only a
    // new, non-zero token confirms the driver observed a connection break.
    if (!recovery || recovery == *lastRecovery)
        return false;
    lastRecovery = recovery;
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

// Once a device has been selected, its own readings govern capture.
template <class Input>
CaptureDeviceRead readCaptureDevice(Input& input, const std::string& deviceId)
{
    CaptureDeviceRead reading;
    input.beginPoll();
    reading.sample = input.sample(deviceId, {}, &reading.status);
    return reading;
}

} // namespace chrome_axis
