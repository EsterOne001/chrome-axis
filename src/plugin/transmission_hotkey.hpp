#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>

namespace chrome_axis {

// Observe only the selected physical key. The input event continues to its destination.
bool startTransmissionHotkey(HMODULE module);
void stopTransmissionHotkey() noexcept;
void selectTransmissionHotkey(unsigned int key) noexcept;
unsigned int transmissionHotkeyBinding() noexcept;
void setTransmissionHotkeyContext(bool eligible) noexcept;
void sampleTransmissionHotkey(unsigned int key, bool pressed) noexcept;
std::optional<std::uint64_t> takeTransmissionHotkeyPress(unsigned int key) noexcept;
bool transmissionHotkeyAvailable() noexcept;

} // namespace chrome_axis
