#pragma once

#include "chrome_axis/diagnostics.hpp"

namespace chrome_axis {

enum class UsbGameStatus { inactive, waitingForGame, waitingForControls, partial, active };

constexpr UsbGameStatus usbGameStatus(bool authorized, bool installed, const DiagnosticRead& report) noexcept
{
    if (!authorized || !installed)
        return UsbGameStatus::inactive;
    if (report.connection != DiagnosticConnection::connected)
        return UsbGameStatus::waitingForGame;
    const auto& snapshot = report.snapshot;
    if (snapshot.mode == DiagnosticMode::usb) {
        if (snapshot.usbReadiness == DiagnosticUsbReadiness::ready)
            return UsbGameStatus::active;
        if (snapshot.usbReadiness == DiagnosticUsbReadiness::partial)
            return UsbGameStatus::partial;
    }
    return UsbGameStatus::waitingForControls;
}

} // namespace chrome_axis
