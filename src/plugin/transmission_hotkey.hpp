#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>

namespace chrome_axis {

enum class TransmissionHotkeySource { hook, poll };

struct TransmissionHotkeyPress {
    std::uint64_t timestamp{};
    TransmissionHotkeySource source{};
};

// Observe only the selected physical key. The input event continues to its destination.
bool startTransmissionHotkey(HMODULE module);
void stopTransmissionHotkey() noexcept;
void selectTransmissionHotkey(unsigned int key) noexcept;
unsigned int transmissionHotkeyBinding() noexcept;
void setTransmissionHotkeyContext(bool eligible) noexcept;
std::uint64_t transmissionHotkeySampleRevision() noexcept;
void sampleTransmissionHotkey(unsigned int key, bool pressed, std::uint64_t observedRevision) noexcept;
std::optional<TransmissionHotkeyPress> takeTransmissionHotkeyPress(unsigned int key) noexcept;
bool transmissionHotkeyAvailable() noexcept;

} // namespace chrome_axis
