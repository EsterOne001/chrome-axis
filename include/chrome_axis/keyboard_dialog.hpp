#pragma once

#define IDD_ACCESSORY_KEYBOARD 208

#ifndef RC_INVOKED
#include <windows.h>

#include <optional>

namespace chrome_axis {

std::optional<unsigned int> chooseAccessoryKey(HWND owner, unsigned int currentKey);

} // namespace chrome_axis
#endif
