#pragma once

#include <windows.h>

#include <optional>

namespace chrome_axis {

// Observe only the physical F8 state. The keyboard message continues to its destination.
bool startTransmissionHotkey(HMODULE module);
void stopTransmissionHotkey() noexcept;
std::optional<bool> transmissionHotkey() noexcept;

} // namespace chrome_axis
