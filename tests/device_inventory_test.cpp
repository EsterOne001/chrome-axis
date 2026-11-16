#include "device_inventory.hpp"
#include "transmission_shortcut.hpp"
#include "usb_game_status.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

namespace {

using namespace chrome_axis;
using namespace std::chrono_literals;

bool waitFor(const auto& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

struct ScanGate {
    std::atomic<unsigned int> started{};
    std::atomic<bool> release{};
    std::atomic<bool> finished{};

    DeviceInventoryResult scan()
    {
        const auto sequence = started.fetch_add(1) + 1;
        if (sequence == 1) {
            release.wait(false);
            finished.store(true);
        }
        DeviceInfo device;
        device.id = std::to_string(sequence);
        return {{std::move(device)}, true};
    }

    void unblock()
    {
        release.store(true);
        release.notify_one();
    }
};

bool testInventory()
{
    auto gate = std::make_shared<ScanGate>();
    DeviceInventory inventory([gate] { return gate->scan(); });
    inventory.refresh();
    const bool started = waitFor([&] { return gate->started.load() == 1; });
    inventory.refreshAfterDeviceChange();
    inventory.refresh();
    const bool waiting = !inventory.current();
    gate->unblock();
    if (!started || !waiting)
        return false;
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;
    const auto result = inventory.current();
    if (!result || result->revision != 3 || result->devices.size() != 1 || result->devices[0].id != "2")
        return false;

    gate = std::make_shared<ScanGate>();
    {
        DeviceInventory closing([gate] { return gate->scan(); });
        closing.refresh();
        if (!waitFor([&] { return gate->started.load() == 1; })) {
            gate->unblock();
            return false;
        }
    }
    const bool closedWhileBlocked = !gate->finished.load();
    gate->unblock();
    return closedWhileBlocked && waitFor([&] { return gate->finished.load(); });
}

bool testShortcut(const std::filesystem::path& root)
{
    constexpr unsigned int customKey = 'L';
    TransmissionShortcut shortcut;
    shortcut.refresh(root);
    if (shortcut.display() != defaultTransmissionHotkey)
        return false;

    std::string error;
    if (!saveTransmissionHotkey(root, customKey, error))
        return false;
    shortcut.refresh(root);
    if (shortcut.saved() != customKey || shortcut.display() != customKey)
        return false;

    std::ofstream(root / transmissionHotkeyFilename) << "invalid\n";
    shortcut.refresh(root);
    if (shortcut.saved() || shortcut.display() != customKey)
        return false;
    TransmissionShortcut reopened;
    reopened.refresh(root);
    if (reopened.display())
        return false;
    reopened.refresh(root, customKey);
    if (reopened.saved() || reopened.display() != customKey)
        return false;
    if (!saveTransmissionHotkey(root, defaultTransmissionHotkey, error))
        return false;
    reopened.refresh(root);
    return reopened.saved() == defaultTransmissionHotkey && reopened.display() == defaultTransmissionHotkey;
}

bool testUsbStatus()
{
    DiagnosticRead report;
    if (usbGameStatus(false, true, report) != UsbGameStatus::inactive ||
        usbGameStatus(true, true, report) != UsbGameStatus::waitingForGame)
        return false;
    report.connection = DiagnosticConnection::connected;
    report.snapshot.mode = DiagnosticMode::usb;
    if (usbGameStatus(true, true, report) != UsbGameStatus::waitingForControls)
        return false;
    report.snapshot.axes[0].state = DiagnosticAxisState::armed;
    if (usbGameStatus(true, true, report) != UsbGameStatus::active)
        return false;
    report.snapshot.mode = DiagnosticMode::gameNotForeground;
    if (usbGameStatus(true, true, report) != UsbGameStatus::waitingForControls)
        return false;
    report.connection = DiagnosticConnection::stale;
    return usbGameStatus(true, true, report) == UsbGameStatus::waitingForGame;
}

} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("chrome-axis-app-state-" + std::to_string(GetCurrentProcessId()));
    const bool inventory = testInventory();
    const bool shortcut = testShortcut(root);
    const bool usbStatus = testUsbStatus();
    std::error_code error;
    std::filesystem::remove(root / transmissionHotkeyFilename, error);
    std::filesystem::remove(root, error);
    if (!inventory || !shortcut || !usbStatus)
        std::cerr << "inventory=" << inventory << " shortcut=" << shortcut << " usb_state=" << usbStatus << '\n';
    return inventory && shortcut && usbStatus ? 0 : 1;
}
