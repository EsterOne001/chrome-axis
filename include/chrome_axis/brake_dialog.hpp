#pragma once

#define IDD_BRAKE_SETTINGS 212
#define IDC_BRAKE_PROGRESSIVITY 2620
#define IDC_BRAKE_SATURATION 2621
#define IDC_BRAKE_RAW_VALUE 2622
#define IDC_BRAKE_ADJUSTED_VALUE 2623
#define IDC_BRAKE_RESET 2624
#define IDC_BRAKE_SIMULATED_TRAVEL 2625
#define IDC_BRAKE_PROGRESSIVITY_LABEL 2626
#define IDC_BRAKE_SATURATION_LABEL 2627
#define IDC_BRAKE_SIMULATION_LABEL 2628
#define IDC_BRAKE_STATUS 2629
#define IDC_BRAKE_RAW_LABEL 2630
#define IDC_BRAKE_ADJUSTED_LABEL 2631
#define IDC_BRAKE_RAW_METER 2632
#define IDC_BRAKE_ADJUSTED_METER 2633

#ifndef RC_INVOKED
#include "chrome_axis/controls.hpp"

namespace chrome_axis {

std::optional<BrakeResponse> editBrakeResponse(HWND owner, DirectInput& input,
    const AxisCalibration& calibration, const BrakeResponse& response);

} // namespace chrome_axis
#endif
