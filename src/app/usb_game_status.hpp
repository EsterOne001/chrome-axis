#pragma once

#include "chrome_axis/diagnostics.hpp"

#include <algorithm>

namespace chrome_axis {

enum class UsbGameStatus { inactive, waitingForGame, waitingForControls, active };

constexpr UsbGameStatus usbGameStatus(bool authorized, bool installed, const DiagnosticRead& report) noexcept
{
    if (!authorized || !installed)
        return UsbGameStatus::inactive;
    if (report.connection != DiagnosticConnection::connected)
        return UsbGameStatus::waitingForGame;
    const auto& snapshot = report.snapshot;
    const bool armed = snapshot.accessoryArmed > 0 || std::ranges::any_of(snapshot.axes,
        [](const DiagnosticAxis& axis) { return axis.state == DiagnosticAxisState::armed; });
    return snapshot.mode == DiagnosticMode::usb && armed ? UsbGameStatus::active : UsbGameStatus::waitingForControls;
}

} // namespace chrome_axis
