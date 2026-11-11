#pragma once

#define IDD_FORCE_FEEDBACK 207
#define IDC_FEEDBACK_STATUS 2701
#define IDC_FEEDBACK_ENABLE 2702
#define IDC_FEEDBACK_STRENGTH 2703
#define IDC_FEEDBACK_IMPACTS 2704
#define IDC_FEEDBACK_DIRECTION 2705

#ifndef RC_INVOKED
#include "chrome_axis/input.hpp"

#include <filesystem>

namespace chrome_axis {

bool editForceFeedback(HWND owner, const std::filesystem::path& directory,
                       const DeviceInfo* steeringDevice, std::size_t steeringAxis);

} // namespace chrome_axis
#endif
