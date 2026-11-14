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

#ifndef RC_INVOKED
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"

namespace chrome_axis {

bool editForceFeedback(HWND owner, const std::filesystem::path& root, const ProfilePreset& preset,
                       const std::optional<ProfileFileRevision>& calibrationRevision,
                       const DeviceInfo* steeringDevice, std::size_t steeringAxis);

} // namespace chrome_axis
#endif
