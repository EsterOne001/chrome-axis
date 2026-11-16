#pragma once

#define IDD_BRAKE_SETTINGS 212
#define IDD_PEDAL_SETTINGS 213
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

#define IDC_PEDAL_PRESET_LABEL 2634
#define IDC_PEDAL_PRESET 2635
#define IDC_PEDAL_BRAKE_GRAPH 2636
#define IDC_PEDAL_THROTTLE_GRAPH 2637
#define IDC_PEDAL_BRAKE_LABEL 2638
#define IDC_PEDAL_THROTTLE_LABEL 2639
#define IDC_PEDAL_RAW_LABEL 2640
#define IDC_PEDAL_CORRECTED_LABEL 2641
#define IDC_PEDAL_GAME_LABEL 2642
#define IDC_PEDAL_BRAKE_RAW 2643
#define IDC_PEDAL_BRAKE_CORRECTED 2644
#define IDC_PEDAL_BRAKE_GAME 2645
#define IDC_PEDAL_THROTTLE_RAW 2646
#define IDC_PEDAL_THROTTLE_CORRECTED 2647
#define IDC_PEDAL_THROTTLE_GAME 2648
#define IDC_PEDAL_BRAKE_TEST_LABEL 2649
#define IDC_PEDAL_BRAKE_TEST 2650
#define IDC_PEDAL_THROTTLE_TEST_LABEL 2651
#define IDC_PEDAL_THROTTLE_TEST 2652
#define IDC_PEDAL_HISTORY 2653
#define IDC_PEDAL_TIME_START 2654
#define IDC_PEDAL_TIME_END 2655
#define IDC_PEDAL_KEY_RAW 2656
#define IDC_PEDAL_KEY_CORRECTED 2657
#define IDC_PEDAL_KEY_GAME 2658
#define IDC_PEDAL_STABILIZATION 2659

#ifndef RC_INVOKED
#include "chrome_axis/controls.hpp"

#include <string_view>

namespace chrome_axis {

class DiagnosticReader;

struct PedalGraphContext {
    const AxisCalibration* otherCalibration{};
    AxisResponse otherResponse{};
    DiagnosticReader& diagnostics;
    std::u8string_view profileName;
};

std::optional<AxisResponse> editAxisResponse(HWND owner, DirectInput& input, ResponseAxis axis,
    const AxisCalibration* calibration, const AxisResponse& response, const PedalGraphContext* pedals = nullptr);

} // namespace chrome_axis
#endif
