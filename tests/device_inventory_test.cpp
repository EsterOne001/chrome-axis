#include "device_capture.hpp"
#include "device_inventory.hpp"
#include "transmission_shortcut.hpp"
#include "usb_game_status.hpp"

#include "chrome_axis/shifter.hpp"

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
    unsigned int blockedScan{1};
    bool keepSelectedDevice{};

    DeviceInventoryResult scan()
    {
        const auto sequence = started.fetch_add(1) + 1;
        if (sequence == blockedScan) {
            release.wait(false);
            finished.store(true);
        }
        DeviceInfo device;
        device.id = std::to_string(sequence);
        if (keepSelectedDevice && sequence > 1) {
            DeviceInfo selected;
            selected.id = "1";
            return {{std::move(selected), std::move(device)}, true};
        }
        return {{std::move(device)}, true};
    }

    void unblock()
    {
        release.store(true);
        release.notify_one();
    }
};

struct CaptureInput {
    DeviceSample report;
    unsigned int cycles{};

    void beginPoll() { ++cycles; }

    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>&,
                                      DeviceReadStatus* status)
    {
        if (id != "1") {
            *status = DeviceReadStatus::unavailable;
            return std::nullopt;
        }
        *status = DeviceReadStatus::available;
        return report;
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

bool testCaptureDuringUnrelatedScan()
{
    auto gate = std::make_shared<ScanGate>();
    gate->blockedScan = 2;
    gate->keepSelectedDevice = true;
    DeviceInventory inventory([gate] { return gate->scan(); });
    inventory.refresh();
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;

    const auto device = inventory.current()->devices.front().id;
    CaptureInput input;
    ShifterCapture capture(device);
    for (int i = 0; i < 3; ++i) {
        const auto reading = readCaptureDevice(input, device);
        if (!reading.sample || reading.needsRest())
            return false;
        capture.captureNeutral(*reading.sample);
    }

    inventory.refreshAfterDeviceChange();
    const bool scanning = waitFor([&] { return gate->started.load() == 2; }) && !inventory.current();
    input.report.buttons.set(1);
    std::optional<AccessoryBinding> binding;
    if (scanning) {
        for (int i = 0; i < 3; ++i) {
            const auto reading = readCaptureDevice(input, device);
            if (!reading.sample || reading.needsRest())
                break;
            binding = capture.sample(*reading.sample).binding;
        }
    }
    const bool capturedBeforeScanFinished = binding && binding->deviceId == device &&
        binding->kind == AccessoryKind::Button && binding->index == 1 && !gate->finished.load();
    gate->unblock();
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;
    const auto updated = inventory.current();
    return capturedBeforeScanFinished && input.cycles == 6 && updated && updated->devices.size() == 2 &&
        updated->devices.front().id == device;
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
    const bool capture = testCaptureDuringUnrelatedScan();
    const bool shortcut = testShortcut(root);
    const bool usbStatus = testUsbStatus();
    std::error_code error;
    std::filesystem::remove(root / transmissionHotkeyFilename, error);
    std::filesystem::remove(root, error);
    if (!inventory || !capture || !shortcut || !usbStatus)
        std::cerr << "inventory=" << inventory << " capture=" << capture << " shortcut=" << shortcut
                  << " usb_state=" << usbStatus << '\n';
    return inventory && capture && shortcut && usbStatus ? 0 : 1;
}
