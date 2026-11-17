#include "app_input.hpp"
#include "device_capture.hpp"
#include "device_inventory.hpp"
#include "transmission_shortcut.hpp"
#include "usb_game_status.hpp"

#include "chrome_axis/calibration.hpp"
#include "chrome_axis/shifter.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

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
    DeviceReadStatus readStatus{DeviceReadStatus::available};
    unsigned int cycles{};

    void beginPoll() { ++cycles; }

    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>&,
                                      DeviceReadStatus* status)
    {
        if (id != "1") {
            *status = DeviceReadStatus::unavailable;
            return std::nullopt;
        }
        *status = readStatus;
        return readStatus == DeviceReadStatus::unavailable ? std::nullopt : std::optional{report};
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

bool testPendingLayout()
{
    auto scans = std::make_shared<std::atomic<unsigned int>>();
    DeviceInventory inventory([scans] {
        const auto count = scans->fetch_add(1) + 1;
        DeviceInfo device;
        device.id = "wheel";
        if (count == 1) {
            device.axes[0] = true;
            device.forceFeedbackAxes[0] = true;
        } else if (count == 2) {
            device.layoutPending.axes.set(0).set(1);
        } else {
            device.axes[1] = true;
        }
        return DeviceInventoryResult{{std::move(device)}, true};
    });
    inventory.refresh();
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;
    const auto known = inventory.current();
    inventory.refresh();
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;
    const auto pending = inventory.current();
    if (!known || !pending || pending->devices.size() != 1 || pending->revision <= known->revision)
        return false;
    const auto& held = pending->devices.front();
    if (!held.axes[0] || !held.forceFeedbackAxes[0] || held.axes[1] ||
        !held.layoutPending.axes[0] || !held.layoutPending.axes[1])
        return false;
    return waitFor([&] {
        const auto result = inventory.current();
        return result && result->revision > pending->revision && result->devices.size() == 1 &&
            !result->devices.front().axes[0] && result->devices.front().axes[1] &&
            result->devices.front().layoutPending.axes.none();
    });
}

struct SampleGate {
    std::atomic<unsigned int> sampleCount{};
    std::atomic<bool> released{}, extraReleased{}, stoppingStarted{}, stoppingReleased{}, stoppingFinished{};

    static void release(std::atomic<bool>& flag)
    {
        flag.store(true);
        flag.notify_one();
    }
};

bool testAsynchronousCapture()
{
    auto gate = std::make_shared<SampleGate>();
    auto input = std::make_unique<AppInput>([gate](const std::string& id, const auto&) {
        DeviceSample sample;
        sample.axesPresent[0] = true;
        if (id == "wheel") {
            const auto read = gate->sampleCount.fetch_add(1) + 1;
            if (read == 1)
                gate->released.wait(false);
            if (read > 2)
                gate->extraReleased.wait(false);
            sample.axes[0] = read == 1 ? 100 : 200;
        } else {
            gate->stoppingStarted.store(true);
            gate->stoppingReleased.wait(false);
            gate->stoppingFinished.store(true);
        }
        return CaptureDeviceRead{sample, DeviceReadStatus::available};
    });

    const auto first = input->capture("wheel");
    const bool started = waitFor([&] { return gate->sampleCount.load() == 1; });
    input->invalidate();
    const auto duringOld = input->capture("wheel");
    SampleGate::release(gate->released);

    CaptureDeviceRead newDevice;
    const bool freshAfterReset = waitFor([&] {
        newDevice = input->capture("wheel");
        return newDevice.sample.has_value();
    }) && newDevice.updated && newDevice.sample->axes[0] == 200 && newDevice.sample->sequence;
    const auto repeated = input->capture("wheel");
    input->invalidate();
    SampleGate::release(gate->extraReleased);

    input->capture("hold");
    const bool closing = waitFor([&] { return gate->stoppingStarted.load(); });
    const auto stillWaiting = input->capture("hold");
    input.reset();
    const bool closedBeforeDriver = !gate->stoppingFinished.load();
    SampleGate::release(gate->stoppingReleased);
    return started && !first.updated && !duringOld.updated && freshAfterReset && !repeated.updated &&
        closing && !stillWaiting.updated && closedBeforeDriver &&
        waitFor([&] { return gate->stoppingFinished.load(); });
}

bool testIndependentDeviceCapture()
{
    auto wheelReads = std::make_shared<std::atomic<unsigned int>>();
    auto releaseWheel = std::make_shared<std::atomic<bool>>();
    AppInput input([wheelReads, releaseWheel](const std::string& id, const auto&) {
        DeviceSample sample;
        sample.axesPresent[0] = true;
        sample.sampledAt = DeviceReadClock::now();
        if (id == "wheel") {
            const auto count = wheelReads->fetch_add(1) + 1;
            if (count == 2)
                releaseWheel->wait(false);
            sample.axes[0] = count == 1 ? 100 : count == 2 ? 900 : 300;
        } else {
            sample.axes[0] = 700;
        }
        return CaptureDeviceRead{sample, DeviceReadStatus::available};
    });

    const bool firstWheel = waitFor([&] {
        const auto reading = input.capture("wheel");
        return reading.sample && reading.sample->axes[0] == 100;
    });
    const bool wheelBlocked = waitFor([&] { return wheelReads->load() == 2; });
    const bool wheelExpired = waitFor([&] {
        const auto reading = input.capture("wheel");
        return reading.updated && !reading.sample && reading.status == DeviceReadStatus::unavailable;
    });
    const auto freshPedals = [&] {
        const auto reading = input.capture("pedals");
        return reading.updated && reading.sample && reading.sample->axes[0] == 700;
    };
    const bool beforePause = waitFor(freshPedals);
    input.close();
    const auto afterPause = input.capture("wheel");
    input.invalidate();
    const auto afterRefresh = input.capture("wheel");
    const bool pedalsResumed = waitFor(freshPedals);
    const bool oneOwner = wheelReads->load() == 2;

    SampleGate::release(*releaseWheel);
    CaptureDeviceRead wheel;
    const bool recovered = waitFor([&] {
        wheel = input.capture("wheel");
        return wheel.sample.has_value();
    }) && wheel.sample->axes[0] == 300;
    return firstWheel && wheelBlocked && wheelExpired && beforePause && !afterPause.updated &&
        !afterRefresh.updated && pedalsResumed && oneOwner && recovered;
}

bool testReaderRecoveryAfterFailure()
{
    auto gate = std::make_shared<SampleGate>();
    AppInput input([gate](const std::string& id, const auto&) {
        DeviceSample sample;
        sample.axesPresent[0] = true;
        if (id == "wheel") {
            const auto read = gate->sampleCount.fetch_add(1) + 1;
            if (read == 2) {
                gate->released.wait(false);
                throw std::runtime_error("simulated reader failure");
            }
            sample.axes[0] = read == 1 ? 100 : 300;
        } else {
            sample.axes[0] = 700;
        }
        return CaptureDeviceRead{sample, DeviceReadStatus::available};
    });

    const bool firstWheel = waitFor([&] {
        const auto reading = input.capture("wheel");
        return reading.sample && reading.sample->axes[0] == 100;
    });
    const bool failedReadStarted = waitFor([&] { return gate->sampleCount.load() == 2; });
    SampleGate::release(gate->released);
    const bool invalidated = waitFor([&] {
        const auto reading = input.capture("wheel");
        return reading.updated && !reading.sample && reading.status == DeviceReadStatus::unavailable;
    });
    const bool noEarlyRetry = gate->sampleCount.load() == 2;
    const bool pedalsReady = waitFor([&] {
        const auto reading = input.capture("pedals");
        return reading.sample && reading.sample->axes[0] == 700;
    });
    CaptureDeviceRead recovered;
    const bool sameDeviceResumed = waitFor([&] {
        recovered = input.capture("wheel");
        return recovered.sample.has_value();
    }) && recovered.sample->axes[0] == 300 && recovered.status == DeviceReadStatus::resumed;
    return firstWheel && failedReadStarted && invalidated && noEarlyRetry && pedalsReady && sameDeviceResumed;
}

bool testIdleReaderRestartsAtNeutral()
{
    auto workers = std::make_shared<std::atomic<unsigned int>>();
    auto position = std::make_shared<std::atomic<long>>(500);
    AppInput input([workers, position](const std::string&, const auto&) {
        thread_local bool sampled{};
        if (!std::exchange(sampled, true))
            ++(*workers);
        DeviceSample sample;
        sample.axesPresent[0] = true;
        sample.axes[0] = position->load();
        return CaptureDeviceRead{sample, DeviceReadStatus::available};
    });
    Profile profile{};
    profile.steering = {"wheel", 0, 500, 0, 1000, 0.01f};
    ControlReader controls(profile);
    if (!waitFor([&] { return controls.poll(input).steeringReady; }) || workers->load() != 1)
        return false;

    // Allow the idle reader to release its resources without destroying AppInput or ControlReader.
    std::this_thread::sleep_for(5500ms);
    position->store(900);
    const bool unavailableBeforeFirstRead = !controls.poll(input).steeringReady &&
        controls.axisStatuses()[0] == AxisStatus::deviceUnavailable;
    const bool waitingForNeutral = waitFor([&] {
        return !controls.poll(input).steeringReady && controls.axisStatuses()[0] == AxisStatus::waitingForNeutral;
    });
    position->store(500);
    return unavailableBeforeFirstRead && waitingForNeutral && workers->load() == 2 &&
        waitFor([&] { return controls.poll(input).steeringReady; });
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
    CalibrationCapture calibration;
    if (scanning) {
        for (int i = 0; i < 3; ++i) {
            const auto reading = readCaptureDevice(input, device);
            if (!reading.sample || reading.needsRest())
                break;
            binding = capture.sample(*reading.sample).binding;
        }
        input.report.axesPresent[0] = true;
        input.report.axes[0] = 31000;
        for (std::size_t i = 0; i < CalibrationCapture::sampleCount; ++i) {
            const auto reading = readCaptureDevice(input, device);
            if (!reading.sample || reading.needsRest() || !calibration.add(*reading.sample))
                break;
        }
    }
    const auto position = calibration.result();
    const bool capturedBeforeScanFinished = binding && binding->deviceId == device &&
        binding->kind == AccessoryKind::Button && binding->index == 1 &&
        position && position->median[0] == 31000 && !gate->finished.load();
    input.readStatus = DeviceReadStatus::unavailable;
    const auto disconnected = readCaptureDevice(input, device);
    input.readStatus = DeviceReadStatus::resumed;
    const auto resumed = readCaptureDevice(input, device);
    gate->unblock();
    if (!waitFor([&] { return static_cast<bool>(inventory.current()); }))
        return false;
    const auto updated = inventory.current();
    return capturedBeforeScanFinished && !disconnected.sample && disconnected.needsRest() &&
        resumed.sample && resumed.needsRest() && input.cycles == 8 + CalibrationCapture::sampleCount &&
        updated && updated->devices.size() == 2 && updated->devices.front().id == device;
}

bool testGuidedCalibrationWaitsForLayout()
{
    CalibrationCapture capture;
    bool hadSignal{};
    DeviceSample sample;
    sample.axesPending.set(0);
    const CaptureDeviceRead pending{sample, DeviceReadStatus::available};
    if (!collectGuidedCalibrationReading(capture, pending, hadSignal) ||
        hadSignal || capture.samplesCollected() != 0)
        return false;
    const CaptureDeviceRead absent{DeviceSample{}, DeviceReadStatus::available};
    if (collectGuidedCalibrationReading(capture, absent, hadSignal))
        return false;

    sample.axesPending.reset(0);
    sample.axesPending.set(1);
    sample.axesPresent[0] = true;
    sample.axes[0] = 31000;
    const CaptureDeviceRead ready{sample, DeviceReadStatus::available};
    if (!collectGuidedCalibrationReading(capture, ready, hadSignal) || !hadSignal ||
        capture.samplesCollected() != 1)
        return false;
    if (!collectGuidedCalibrationReading(capture, {.status = DeviceReadStatus::pending, .updated = false}, hadSignal) ||
        capture.samplesCollected() != 1)
        return false;
    if (collectGuidedCalibrationReading(capture, pending, hadSignal) ||
        collectGuidedCalibrationReading(capture, {.status = DeviceReadStatus::unavailable}, hadSignal) ||
        collectGuidedCalibrationReading(capture, {sample, DeviceReadStatus::resumed}, hadSignal))
        return false;
    auto lostAxis = sample;
    lostAxis.axesPresent[0] = false;
    lostAxis.axesPresent[1] = true;
    lostAxis.axesPending.reset(1).set(0);
    if (collectGuidedCalibrationReading(capture, {lostAxis, DeviceReadStatus::available}, hadSignal))
        return false;
    sample.axesPending.reset(1);
    sample.axesPresent[1] = true;
    const CaptureDeviceRead expanded{sample, DeviceReadStatus::available};
    for (std::size_t i = 1; i < CalibrationCapture::sampleCount; ++i)
        if (!collectGuidedCalibrationReading(capture, expanded, hadSignal))
            return false;
    const auto completed = capture.result();
    return completed && completed->median[0] == sample.axes[0] && !completed->axesPresent[1];
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
    report.snapshot.accessoryArmed = 1;
    if (usbGameStatus(true, true, report) != UsbGameStatus::waitingForControls)
        return false;
    report.snapshot.usbReadiness = DiagnosticUsbReadiness::partial;
    if (usbGameStatus(true, true, report) != UsbGameStatus::partial)
        return false;
    report.snapshot.usbReadiness = DiagnosticUsbReadiness::ready;
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
    const bool pendingLayout = testPendingLayout();
    const bool asyncCapture = testAsynchronousCapture();
    const bool independentCapture = testIndependentDeviceCapture();
    const bool readerRecovery = testReaderRecoveryAfterFailure();
    const bool idleReader = testIdleReaderRestartsAtNeutral();
    const bool capture = testCaptureDuringUnrelatedScan();
    const bool guidedCalibration = testGuidedCalibrationWaitsForLayout();
    const bool shortcut = testShortcut(root);
    const bool usbStatus = testUsbStatus();
    std::error_code error;
    std::filesystem::remove(root / transmissionHotkeyFilename, error);
    std::filesystem::remove(root, error);
    if (!inventory || !pendingLayout || !asyncCapture || !independentCapture || !readerRecovery || !idleReader || !capture ||
        !guidedCalibration || !shortcut || !usbStatus)
        std::cerr << "inventory=" << inventory << " pending_layout=" << pendingLayout << " async_capture=" << asyncCapture
                  << " independent_capture=" << independentCapture << " reader_recovery=" << readerRecovery
                  << " idle_reader=" << idleReader << " capture=" << capture
                  << " guided_calibration=" << guidedCalibration << " shortcut=" << shortcut
                  << " usb_state=" << usbStatus << '\n';
    return inventory && pendingLayout && asyncCapture && independentCapture && readerRecovery && idleReader && capture &&
        guidedCalibration && shortcut && usbStatus ? 0 : 1;
}
