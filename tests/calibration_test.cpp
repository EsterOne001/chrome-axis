#include "chrome_axis/calibration.hpp"

#include <cmath>
#include <iostream>

namespace {

int run()
{
    chrome_axis::CalibrationCapture capture;
    chrome_axis::DeviceSample sample;
    if (capture.add(sample) || capture.complete() || capture.result())
        return 1;

    sample.axesPresent[0] = true;
    for (std::size_t i = 0; i < chrome_axis::CalibrationCapture::sampleCount; ++i) {
        sample.axes[0] = 1000 - static_cast<long>(i);
        if (i == 0)
            sample.axes[0] = 100000;
        else if (i == chrome_axis::CalibrationCapture::sampleCount - 1)
            sample.axes[0] = -100000;
        if (!capture.add(sample))
            return 2;
        if (i == 0) {
            auto changed = sample;
            changed.axesPresent[1] = true;
            if (capture.add(changed) || capture.result())
                return 3;
        }
    }
    const auto measured = capture.result();
    if (!capture.complete() || !measured || measured->median[0] != 993 || measured->spread[0] != 12
        || !measured->axesPresent[0] || measured->axesPresent[1] || capture.add(sample))
        return 4;

    chrome_axis::AxisCapture rest;
    chrome_axis::AxisCapture left;
    chrome_axis::AxisCapture right;
    rest.axesPresent = {true, true, true, true, true, false, true, true};
    left.axesPresent = rest.axesPresent;
    right.axesPresent = rest.axesPresent;
    rest.median = {500, 500, 500, 500, 500, 500, 500, 500};
    left.median = {200, 200, 900, 450, 200, 0, 100, 300};
    right.median = {800, 800, 950, 700, 900, 1500, 1000, 800};
    right.spread[4] = 100;
    rest.spread[6] = 10;
    right.axesPresent[7] = false;
    const auto steering = chrome_axis::axisCandidates(rest, left, right, true);
    if (steering.size() != 4 || steering[0].axis != 6 || steering[0].travel != 400
        || std::abs(steering[0].deadzone - 0.05f) > 0.0001f || steering[1].axis != 0
        || steering[2].axis != 1 || steering[3].axis != 3 || steering[3].travel != 50)
        return 5;

    chrome_axis::AxisCapture pedalRest;
    chrome_axis::AxisCapture pedalDown;
    pedalRest.axesPresent[2] = pedalDown.axesPresent[2] = true;
    pedalRest.median[2] = 65000;
    pedalDown.median[2] = 9000;
    const auto pedals = chrome_axis::axisCandidates(pedalRest, pedalRest, pedalDown, false);
    if (pedals.size() != 1 || pedals[0].axis != 2 || pedals[0].travel != 56000
        || std::abs(pedals[0].deadzone - 0.03f) > 0.0001f)
        return 6;
    const auto pedal = chrome_axis::makeCalibration("pedal", pedalRest, pedalRest, pedalDown, pedals[0]);
    if (pedal.deviceId != "pedal" || pedal.axis != 2 || pedal.neutral != 65000
        || pedal.negative != 65000 || pedal.positive != 9000)
        return 7;
    return 0;
}

} // namespace

int main()
{
    const int result = run();
    if (result != 0)
        std::cerr << "Calibration capture regression, check " << result << '\n';
    return result;
}
