#pragma once

#define IDD_FORCE_FEEDBACK 207
#define IDC_FEEDBACK_STATUS 2701
#define IDC_FEEDBACK_ENABLE 2702
#define IDC_FEEDBACK_STRENGTH 2703
#define IDC_FEEDBACK_IMPACTS 2704
#define IDC_FEEDBACK_DIRECTION 2705
#define IDC_FEEDBACK_ENABLE_LABEL 2706
#define IDC_FEEDBACK_STRENGTH_LABEL 2707
#define IDC_FEEDBACK_IMPACTS_LABEL 2708
#define IDC_FEEDBACK_DIRECTION_LABEL 2709
#define IDC_FEEDBACK_STRENGTH_VALUE 2710
#define IDC_FEEDBACK_CENTERING 2711
#define IDC_FEEDBACK_CENTERING_LABEL 2712
#define IDC_FEEDBACK_CENTERING_VALUE 2713
#define IDC_FEEDBACK_IMPACT_STRENGTH 2714
#define IDC_FEEDBACK_IMPACT_STRENGTH_LABEL 2715
#define IDC_FEEDBACK_IMPACT_STRENGTH_VALUE 2716
#define IDC_FEEDBACK_LOW_SPEED 2717
#define IDC_FEEDBACK_LOW_SPEED_LABEL 2718
#define IDC_FEEDBACK_LOW_SPEED_VALUE 2719
#define IDC_FEEDBACK_DAMPING 2720
#define IDC_FEEDBACK_DAMPING_LABEL 2721
#define IDC_FEEDBACK_DAMPING_VALUE 2722
#define IDC_FEEDBACK_SUGGESTION 2723
#define IDC_FEEDBACK_USE_SUGGESTION 2724

#ifndef RC_INVOKED
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"

namespace chrome_axis {

bool editForceFeedback(HWND owner, const std::filesystem::path& root, const ProfilePreset& preset,
                       const std::optional<ProfileFileRevision>& calibrationRevision,
                       const DeviceInfo* steeringDevice, std::size_t steeringAxis);

} // namespace chrome_axis
#endif
