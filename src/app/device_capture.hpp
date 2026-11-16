#pragma once

#include "chrome_axis/input.hpp"

namespace chrome_axis {

struct CaptureDeviceRead {
    std::optional<DeviceSample> sample;
    DeviceReadStatus status{DeviceReadStatus::unavailable};

    bool needsRest() const noexcept {
        return status == DeviceReadStatus::unavailable || status == DeviceReadStatus::resumed;
    }
};

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
