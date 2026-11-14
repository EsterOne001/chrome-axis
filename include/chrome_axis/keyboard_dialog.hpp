#pragma once

#define IDD_ACCESSORY_KEYBOARD 208
#define IDC_KEYBOARD_NAVIGATION_LABEL 2801
#define IDC_KEYBOARD_NUMPAD_LABEL 2802
#define IDC_KEYBOARD_NUMLOCK_HINT 2803

#ifndef RC_INVOKED
#include <windows.h>

#include <optional>

namespace chrome_axis {

std::optional<unsigned int> chooseAccessoryKey(HWND owner, unsigned int currentKey);

} // namespace chrome_axis
#endif
