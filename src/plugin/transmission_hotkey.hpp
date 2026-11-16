#pragma once

#include <windows.h>

#include <optional>

namespace chrome_axis {

// Observe only the selected physical key. The input event continues to its destination.
bool startTransmissionHotkey(HMODULE module);
void stopTransmissionHotkey() noexcept;
void selectTransmissionHotkey(unsigned int key) noexcept;
unsigned int transmissionHotkeyBinding() noexcept;
std::optional<bool> transmissionHotkey(unsigned int key) noexcept;

} // namespace chrome_axis
