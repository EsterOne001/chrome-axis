#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/configuration.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"chrome-axis-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove(path / L"one", error);
        std::filesystem::remove(path / L"two", error);
        std::filesystem::remove(path, error);
    }
};

bool checkPercentiles()
{
    chrome_axis::PerformanceTiming timing;
    if (timing.percentile(99))
        return false;
    for (int i = 0; i < 95; ++i)
        timing.add(250);
    for (int i = 0; i < 3; ++i)
        timing.add(2100);
    timing.add(14000);
    timing.add(2000000);
    return timing.samples == 100 && timing.percentile(95) == 250 &&
        timing.percentile(99) == 15000 && timing.percentile(100) == 2000000;
}

int testPerformanceCapture(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    DiagnosticReader owner(root);
    DiagnosticReader observer(root);
    DiagnosticSnapshot snapshot;
    snapshot.mode = DiagnosticMode::gameNotForeground;
    if (owner.requestPerformanceCapture(true))
        return 13;
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot);
        owner.read();
        observer.read();
        const auto start = owner.requestPerformanceCapture(true);
        const auto startRequest = publisher.performanceRequest();
        if (!start || !owner.ownsPerformanceCapture() || !startRequest || startRequest->id != start ||
            !startRequest->enabled || startRequest->leaseUntil <= GetTickCount64() ||
            observer.requestPerformanceCapture(false) || observer.requestPerformanceCapture(true))
            return 14;

        PerformanceReport performance;
        performance.state = PerformanceState::capturing;
        performance.requestId = start;
        performance.startedAt = GetTickCount64();
        performance.usbRead.add(300);
        performance.usbRead.add(500);
        performance.feedback.add(200);
        performance.inputAge.add(1500);
        performance.motionAge.add(4500);
        performance.motionAge.add(18500);
        performance.expiredReads = 2;
        performance.transientReads = 3;
        performance.forceSamples = 20;
        performance.clippedSamples = 3;
        performance.impactClippedSamples = 2;
        performance.submittedEffects = 13;
        performance.peakRequestedForce = 1.625f;
        performance.peakLimitedForce = 1.0f;
        performance.peakSubmittedForce = 0.875f;
        performance.usbSlowReads = 4;
        performance.usbLateStarts = 9;
        performance.feedbackIntervalOverruns = 5;
        performance.peakContinuousForce = 1.125f;
        performance.referenceFeedbackGain = 130;
        performance.feedbackSetupSignature = 0xB27D4A4520D8C789ull;
        publisher.publish(snapshot, 1, performance);
        DrivingCaptureSample sample;
        sample.sampledAt = performance.startedAt;
        sample.mode = DiagnosticMode::usb;
        constexpr std::string_view name = "Car, \"A\"";
        std::ranges::copy(name, sample.vehicle.begin());
        sample.speedMps = 12.5f;
        sample.speedAvailable = true;
        sample.rpm = 3450.0f; // The missing flag must keep this field blank.
        sample.throttle = 0.5f;
        sample.throttleAvailable = true;
        sample.brakeAvailable = true;
        sample.transmission = {
            .configured = true, .manual = true, .selectedGear = 3, .requestedGear = 3, .engagedGear = 2,
            .shift = DiagnosticShiftState::pending, .sequential = true, .shortcutKey = VK_F8,
            .shortcutPressedAt = sample.sampledAt, .shortcutPressCount = 1, .shortcutSource = DiagnosticShortcutSource::hook,
            .hasRequestedMode = true, .requestedManual = true,
            .shortcutAvailable = true, .gearboxSampledAt = sample.sampledAt,
        };
        sample.clutch = {0.7f, 0.4f, true, true, true, false, true, true};
        publisher.recordDrivingSample(start + 1, sample);
        publisher.recordDrivingSample(start, sample);
        sample.sampledAt += 5;
        sample.transmission.shortcutPressedAt = sample.sampledAt;
        sample.transmission.gearboxSampledAt = sample.sampledAt - 2;
        publisher.recordDrivingSample(start, sample); // Timestamps alone do not force a row.
        sample.transmission.shortcutPressCount += 2;
        publisher.recordDrivingSample(start, sample); // Two presses are visible even when the mode is unchanged.
        const auto availableClutch = sample.clutch;
        sample.clutch.inputAvailable = false;
        sample.clutch.signal = DiagnosticCommandState::temporarilyUnavailable;
        sample.clutch.blocker = DiagnosticClutchBlocker::signalPending;
        publisher.recordDrivingSample(start, sample); // A blocker change keeps its own row.
        sample.clutch = availableClutch;
        sample.sampledAt += 5;
        sample.transmission.shortcutPressedAt = sample.sampledAt;
        ++sample.transmission.shortcutPressCount;
        sample.transmission.shortcutSource = DiagnosticShortcutSource::poll;
        sample.transmission.engagedGear = 3;
        sample.transmission.shift = DiagnosticShiftState::accepted;
        sample.shiftDelayMicros = 9900;
        sample.shiftDelayAvailable = true;
        publisher.recordDrivingSample(start, sample); // Rapid shift transition is retained.
        sample.sampledAt += 20;
        sample.mode = DiagnosticMode::usbDisabled;
        sample.transmission = {};
        sample.clutch = {};
        sample.shiftDelayAvailable = false;
        publisher.recordDrivingSample(start, sample); // Native driving with USB disabled is a valid baseline.
        sample.sampledAt += 20;
        sample.mode = DiagnosticMode::waitingForVehicleInput;
        publisher.recordDrivingSample(start, sample); // Pausing is a context transition.
        sample.sampledAt += 10;
        sample.transmission.shortcutKey = 'L';
        publisher.recordDrivingSample(start, sample); // A new binding can arrive during the same menu.
        sample.sampledAt += 10;
        sample.transmission = {.configured = true, .manual = true, .selectedGear = 2, .requestedGear = 2,
                               .engagedGear = 2, .shift = DiagnosticShiftState::accepted, .shortcutKey = 'L',
                               .hasRequestedMode = true, .requestedManual = true};
        publisher.recordDrivingSample(start, sample); // A mode change alone also gets its own row.
        sample.sampledAt += 20;
        sample.transmission.gearboxSampledAt = sample.sampledAt - 3;
        publisher.recordDrivingSample(start, sample); // A timestamp alone leaves the menu state unchanged.
        publisher.publish(snapshot, 1, performance);
        const auto live = observer.read();
        if (observer.ownsPerformanceCapture() || live.performance.state != PerformanceState::capturing ||
            live.performance.requestId != start || live.performance.usbRead.samples != 2 ||
            live.performance.usbRead.totalMicros != 800 || live.performance.usbRead.maxMicros != 500 ||
            live.performance.feedback.totalMicros != 200 || live.performance.inputAge.maxMicros != 1500 ||
            live.performance.expiredReads != 2 || live.performance.transientReads != 3 ||
            live.performance.motionAge.samples != 2 || live.performance.motionAge.totalMicros != 23000 ||
            live.performance.motionAge.maxMicros != 18500 || live.performance.forceSamples != 20 ||
            live.performance.clippedSamples != 3 || live.performance.impactClippedSamples != 2 ||
            live.performance.submittedEffects != 13 || live.performance.peakRequestedForce != 1.625f ||
            live.performance.peakLimitedForce != 1.0f || live.performance.peakSubmittedForce != 0.875f ||
            live.performance.usbSlowReads != 4 || live.performance.usbLateStarts != 9 ||
            live.performance.feedbackIntervalOverruns != 5 ||
            live.performance.peakContinuousForce != 1.125f || live.performance.referenceFeedbackGain != 130 ||
            live.performance.feedbackSetupSignature != 0xB27D4A4520D8C789ull ||
            live.drivingCaptureSamples || owner.read().drivingCaptureSamples != 5)
            return 15;

        std::array<DiagnosticCaptureRecord, 9> records;
        const auto firstBatch = owner.drainCapture(std::span(records).first(3));
        if (!firstBatch.available || firstBatch.finished || firstBatch.dropped || firstBatch.count != 3 ||
            observer.drainCapture(records).available)
            return 28;

        // Leave the incident queued at the publisher so the final snapshot must flush it.
        DiagnosticUsbEvent usb{.sampledAt = sample.sampledAt, .durationMicros = 52100, .result = 0x80070005,
                              .kind = DiagnosticUsbEventKind::deviceSlow, .mode = sample.mode};
        constexpr std::string_view privateId = "private-device-identifier";
        constexpr std::string_view operation = "Poll";
        std::ranges::copy(privateId, usb.deviceId.begin());
        std::ranges::copy(operation, usb.operation.begin());
        publisher.recordUsbEvent(usb);
        const auto stop = owner.requestPerformanceCapture(false);
        const auto stopRequest = publisher.performanceRequest();
        if (stop <= start || owner.ownsPerformanceCapture() || !stopRequest || stopRequest->id != stop ||
            stopRequest->enabled || stopRequest->leaseUntil || observer.requestPerformanceCapture(true))
            return 16;
        sample.sampledAt += 50;
        publisher.recordDrivingSample(start, sample); // No late samples after the owner stops.
        performance.requestId = stop;
        performance.state = PerformanceState::complete;
        performance.endedAt = GetTickCount64();
        publisher.publish(snapshot, 2, performance);
        const auto finished = owner.read().performance;
        if (finished.state != PerformanceState::complete || finished.requestId != stop ||
            finished.startedAt != performance.startedAt || finished.endedAt != performance.endedAt ||
            finished.usbRead.totalMicros != 800 || finished.motionAge.totalMicros != 23000 ||
            finished.clippedSamples != 3 || finished.impactClippedSamples != 2 || finished.submittedEffects != 13)
            return 17;
        const auto tail = owner.drainCapture(std::span(records).subspan(3));
        if (!tail.available || !tail.finished || tail.count != 6 || tail.dropped ||
            records.front().driving.transmission.shift != DiagnosticShiftState::pending ||
            records[1].driving.transmission.shortcutPressCount != 3 ||
            records[1].driving.transmission.shortcutSource != DiagnosticShortcutSource::hook ||
            records[1].driving.transmission.manual != records[0].driving.transmission.manual ||
            records[2].driving.clutch.blocker != DiagnosticClutchBlocker::signalPending ||
            records[2].driving.clutch.inputAvailable ||
            records[3].driving.transmission.shortcutPressedAt != performance.startedAt + 10 ||
            records[3].driving.transmission.shortcutSource != DiagnosticShortcutSource::poll ||
            records[3].driving.shiftDelayMicros != 9900 || records[4].driving.mode != DiagnosticMode::usbDisabled ||
            !records[4].driving.speedAvailable || records[4].driving.speedMps != 12.5f ||
            records[5].driving.mode != DiagnosticMode::waitingForVehicleInput ||
            records[6].driving.mode != DiagnosticMode::waitingForVehicleInput ||
            records[6].driving.transmission.shortcutKey != 'L' ||
            records[6].driving.transmission.configured || records[7].driving.transmission.engagedGear != 2 ||
            !records[8].usbIncident || records[8].usb.durationMicros != 52100 ||
            owner.drainCapture(records).count || observer.read().drivingCaptureSamples ||
            observer.drainCapture(records).available)
            return 22;
        std::ostringstream csv;
        writeDiagnosticCaptureHeader(csv);
        writeDiagnosticCaptureRows(csv, std::span(records).first(3), performance.startedAt);
        writeDiagnosticCaptureRows(csv, std::span(records).subspan(3), performance.startedAt);
        writeDiagnosticCaptureConnection(csv, performance.startedAt, performance.startedAt + 100, false);
        writeDiagnosticCaptureConnection(csv, performance.startedAt, performance.startedAt + 200, true);
        const auto text = csv.str();
        constexpr std::string_view pendingRow =
            "\r\n0,usb,\"Car, \"\"A\"\"\",12.5,,0.5,0,1,0.7,1,0,1,0.4,manual,sequential,3,3,2,pending,,1,0,1,1,1,1,0,driving,";
        constexpr std::string_view acceptedRow =
            "\r\n10,usb,\"Car, \"\"A\"\"\",12.5,,0.5,0,1,0.7,1,0,1,0.4,manual,sequential,3,3,3,accepted,9900,1,0,1,1,1,1,1,driving,";
        if (!text.starts_with("elapsed_ms,mode,vehicle,speed_mps,engine_rpm,") ||
            text.find(pendingRow) == std::string::npos || text.find(acceptedRow) == std::string::npos ||
            text.find(",transmission_shortcut_press_count,transmission_shortcut_source,") == std::string::npos ||
            text.find(",119,1,0,1,hook,manual,0,inactive,,unknown,none,,disabled\r\n") == std::string::npos ||
            text.find(",119,1,5,3,hook,manual,2,inactive,,unknown,none,,disabled\r\n") == std::string::npos ||
            text.find(",temporarily_unavailable,,unknown,signal_pending,,disabled\r\n") == std::string::npos ||
            text.find(",119,1,10,4,poll,manual,7,inactive,,unknown,none,,disabled\r\n") == std::string::npos ||
            text.find("\r\n30,usb_disabled,") == std::string::npos ||
            text.find("\r\n50,waiting_for_vehicle_input,") == std::string::npos ||
            text.find("\r\n70,waiting_for_vehicle_input,,,,,,,,,,,,manual,,2,2,2,accepted,,,,,,,,,context,") == std::string::npos ||
            text.find(",76,0,,0,,manual,,,,,,,disabled\r\n") == std::string::npos ||
            text.find(",context,") == std::string::npos ||
            text.find(",device_slow,\"Poll\",0x80070005,52100,") == std::string::npos ||
            text.find(privateId) != std::string::npos ||
            text.find(",connection_lost,") == std::string::npos ||
            text.find(",connection_restored,") == std::string::npos) {
            std::cerr << text;
            return 23;
        }

        {
            DiagnosticReader closing(root);
            closing.read();
            if (closing.requestPerformanceCapture(true) <= stop)
                return 18;
        }
        const auto closedRequest = publisher.performanceRequest();
        if (!closedRequest || closedRequest->id <= stop || closedRequest->enabled)
            return 19;

        if (observer.requestPerformanceCapture(true))
            return 20;
    }

    DiagnosticPublisher restarted(root);
    restarted.publish(snapshot);
    const auto restartedRequest = restarted.performanceRequest();
    if (observer.requestPerformanceCapture(false) || observer.read().performance.state != PerformanceState::idle ||
        observer.ownsPerformanceCapture() || !restartedRequest || restartedRequest->id)
        return 21;
    return 0;
}

bool testUsbBlockerRecording(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    DiagnosticPublisher publisher(root);
    DiagnosticReader reader(root);
    const auto tick = GetTickCount64();
    DiagnosticSnapshot first{.mode = DiagnosticMode::usb};
    first.transmission = {
        .configured = true, .engagedGear = 2, .shortcutKey = VK_F8, .shortcutPressedAt = tick,
        .hasRequestedMode = true, .requestedManual = true, .shortcutAvailable = true,
    };
    first.transmission.shift = DiagnosticShiftState::clutchRequired;
    first.clutch = {.configured = true, .assistanceEnabled = true, .rawTravel = -0.034f,
        .signal = DiagnosticCommandState::waitingForNeutral, .rest = DiagnosticClutchRest::outOfRange,
        .blocker = DiagnosticClutchBlocker::waitingForNeutral};
    first.axes[1] = {.state = DiagnosticAxisState::armed, .value = 0.6f, .appliedAt = tick};
    first.usbReadiness = DiagnosticUsbReadiness::partial;
    first.feedback = DiagnosticFeedbackState::settingsLocked;
    first.axes[2] = {.state = DiagnosticAxisState::waitingForNeutral,
        .pedalRest = {-0.006, minimumPedalDeadzone, PedalRestState::outsideRest, true}};
    publisher.publish(first, 1);
    const auto initial = reader.read();
    if (!initial.lastDriveAt || initial.lastDrive.transmission != first.transmission)
        return false;
    if (initial.snapshot.usbReadiness != DiagnosticUsbReadiness::partial ||
        initial.snapshot.feedback != DiagnosticFeedbackState::settingsLocked ||
        initial.snapshot.axes[2].pedalRest.state != PedalRestState::outsideRest ||
        initial.snapshot.axes[2].pedalRest.position != -0.006 || !initial.snapshot.axes[2].pedalRest.symmetric)
        return false;

    DiagnosticSnapshot waiting{.mode = DiagnosticMode::waitingForVehicleInput};
    waiting.feedback = DiagnosticFeedbackState::waitingForMotion;
    waiting.transmission.shortcutKey = 'L';
    waiting.clutch = {.configured = true, .assistanceEnabled = true,
        .signal = DiagnosticCommandState::temporarilyUnavailable, .blocker = DiagnosticClutchBlocker::signalPending};
    publisher.publish(waiting, 1);
    const auto interrupted = reader.read();
    if (interrupted.snapshot.mode != DiagnosticMode::waitingForVehicleInput ||
        interrupted.snapshot.feedback != DiagnosticFeedbackState::waitingForMotion ||
        interrupted.lastDrive.feedback != DiagnosticFeedbackState::settingsLocked ||
        interrupted.snapshot.transmission.shortcutKey != 'L' || interrupted.lastDriveAt != initial.lastDriveAt ||
        interrupted.lastDrive.transmission != first.transmission ||
        interrupted.snapshot.clutch.blocker != DiagnosticClutchBlocker::signalPending ||
        interrupted.snapshot.usbReadiness != DiagnosticUsbReadiness::waiting ||
        interrupted.snapshot.axes[2].pedalRest.state != PedalRestState::unavailable ||
        interrupted.lastDrive.axes[2].pedalRest.threshold != minimumPedalDeadzone ||
        !interrupted.lastDrive.axes[2].pedalRest.symmetric ||
        interrupted.lastDrive.axes[2].pedalRest.state != PedalRestState::outsideRest ||
        interrupted.lastDrive.clutch.blocker != DiagnosticClutchBlocker::waitingForNeutral ||
        interrupted.lastDrive.clutch.rawTravel != -0.034f || interrupted.lastDrive.clutch.inputAvailable)
        return false;

    DiagnosticSnapshot expired{.mode = DiagnosticMode::usb, .accessories = DiagnosticAccessoryState::sampleExpired};
    expired.axes[1].state = DiagnosticAxisState::sampleExpired;
    publisher.publish(expired, 1, {}, false);
    const auto rearming = reader.read();
    if (rearming.snapshot.axes[1].state != DiagnosticAxisState::sampleExpired ||
        rearming.lastDrive.axes[1].appliedAt != tick || rearming.lastDrive.axes[1].value != 0.6f ||
        rearming.lastDrive.transmission != first.transmission || rearming.lastDriveAt != initial.lastDriveAt)
        return false;
    expired.axes[1].state = DiagnosticAxisState::waitingForNeutral;
    expired.accessories = DiagnosticAccessoryState::waiting;
    publisher.publish(expired, 1, {}, false);
    publisher.publish({.mode = DiagnosticMode::gameNotForeground});
    const auto background = reader.read();
    if (background.lastDrive.transmission.engagedGear != 2 || background.lastDrive.axes[1].value != 0.6f ||
        background.lastDrive.feedback != DiagnosticFeedbackState::settingsLocked ||
        background.lastDriveAt != initial.lastDriveAt || background.events[2].axes[1] != DiagnosticAxisState::sampleExpired ||
        background.lastDrive.clutch.rest != DiagnosticClutchRest::outOfRange || background.snapshot.clutch.configured ||
        background.events[3].axes[1] != DiagnosticAxisState::waitingForNeutral)
        return false;

    const DiagnosticUsbEvent blockers{
        .sampledAt = tick + 40, .kind = DiagnosticUsbEventKind::commandsWaiting, .mode = DiagnosticMode::usb,
        .accessories = DiagnosticAccessoryState::modifierBlocked, .accessoryCount = 5,
        .blockers = {{
            {.kind = AccessoryKind::Button, .role = ShifterRole::Gear1, .index = 1,
             .state = DiagnosticCommandState::waitingForNeutral},
            {.kind = AccessoryKind::Axis, .index = 2, .state = DiagnosticCommandState::temporarilyUnavailable,
             .clutch = true},
            {.kind = AccessoryKind::Pov, .key = VK_F8, .state = DiagnosticCommandState::inputUnavailable},
            {.kind = AccessoryKind::Axis, .index = 3, .state = DiagnosticCommandState::sampleExpired,
             .progressiveHandbrake = true},
        }},
        .blockerCount = 5,
    };
    publisher.recordUsbEvent(blockers);
    const auto recordLayout = [&](std::uint64_t sampledAt, DiagnosticUsbEventKind kind,
                                  std::string_view operation, HRESULT result) {
        DiagnosticUsbEvent event{
            .sampledAt = sampledAt, .result = static_cast<std::uint32_t>(result),
            .kind = kind, .mode = DiagnosticMode::usb, .deviceAxisMask = 0b001,
        };
        std::ranges::copy(operation, event.operation.begin());
        publisher.recordUsbEvent(event);
    };
    recordLayout(tick + 41, DiagnosticUsbEventKind::layoutPending, "GetObjectInfo axis", E_PENDING);
    recordLayout(tick + 43, DiagnosticUsbEventKind::layoutRecovered, "Read handle GetObjectInfo axis", S_OK);
    recordLayout(tick + 42, DiagnosticUsbEventKind::layoutAcquireDenied, "Probe Acquire", E_ACCESSDENIED);
    recordLayout(tick + 44, DiagnosticUsbEventKind::layoutMissing, "GetObjectInfo button",
                 HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    first.transmission.engagedGear = -1;
    first.transmission.manual = true;
    publisher.publish(first, 1);
    const auto resumed = reader.read();
    if (resumed.lastDrive.transmission != first.transmission || resumed.eventCount != 6 ||
        resumed.usbEventCount != 5 || resumed.usbEvents[0].blockerCount != blockers.blockerCount ||
        !resumed.usbEvents[0].blockers[1].clutch || !resumed.usbEvents[0].blockers[3].progressiveHandbrake ||
        resumed.usbEvents[2].kind != DiagnosticUsbEventKind::layoutAcquireDenied ||
        resumed.usbEvents[3].kind != DiagnosticUsbEventKind::layoutRecovered)
        return false;

    const std::array rows{
        DiagnosticCaptureRecord{.driving = {.sampledAt = tick, .mode = DiagnosticMode::usb,
                                            .transmission = first.transmission, .clutch = first.clutch,
                                            .feedback = first.feedback}},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[0], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[1], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[2], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[3], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[4], .usbIncident = true},
        DiagnosticCaptureRecord{.driving = {.sampledAt = tick + 50, .mode = waiting.mode,
                                            .transmission = waiting.transmission, .feedback = waiting.feedback}},
        DiagnosticCaptureRecord{.driving = {.sampledAt = tick + 51, .mode = DiagnosticMode::usb,
                                            .transmission = {.configured = true,
                                                .shift = DiagnosticShiftState::selectorAmbiguous}}},
        DiagnosticCaptureRecord{.usb = {.sampledAt = tick + 52, .kind = DiagnosticUsbEventKind::commandsWaiting,
            .mode = DiagnosticMode::usb, .accessories = DiagnosticAccessoryState::selectorAmbiguous}, .usbIncident = true},
        DiagnosticCaptureRecord{.usb = {.sampledAt = tick + 53, .kind = DiagnosticUsbEventKind::discreteOverflow,
            .mode = DiagnosticMode::usb}, .usbIncident = true},
    };
    std::ostringstream csv;
    writeDiagnosticCaptureHeader(csv);
    writeDiagnosticCaptureRows(csv, rows, tick, 1);
    writeDiagnosticCaptureConnection(csv, tick, tick + 60, false);
    const auto text = csv.str();
    constexpr std::string_view blockersCell =
        ",5,gear_1:button:2:0:waiting_for_neutral|clutch:axis:3:0:temporarily_unavailable|"
        "accessory:pov:1:119:input_unavailable|handbrake:axis:4:0:sample_expired,,,,,,,,,,,,,\r\n";
    if (text.find(blockersCell) == std::string::npos || text.find(",76,0,,0,,,,,,,,,waiting_for_motion\r\n") == std::string::npos ||
        text.find(",clutch_signal,clutch_raw_travel,clutch_rest,clutch_shift_blocker,usb_call_lane,feedback_state\r\n") == std::string::npos ||
        text.find(",waiting_for_neutral,-0.034,out_of_range,waiting_for_neutral,,settings_locked\r\n") == std::string::npos ||
        text.find(",clutch_required,") == std::string::npos ||
        text.find(",modifier_blocked,0,5,") == std::string::npos ||
        text.find(",selector_ambiguous,,") == std::string::npos ||
        text.find(",selector_ambiguous,0,0,") == std::string::npos ||
        text.find(",discrete_overflow,") == std::string::npos ||
        text.find(",layout_pending,\"GetObjectInfo axis\",0x8000000A,") == std::string::npos ||
        text.find(",layout_acquire_denied,\"Probe Acquire\",0x80070005,") == std::string::npos ||
        text.find(",layout_recovered,\"Read handle GetObjectInfo axis\",0x00000000,") == std::string::npos ||
        text.find(",layout_missing,\"GetObjectInfo button\",0x80070002,") == std::string::npos)
        return false;
    std::istringstream lines(text);
    std::string row;
    while (std::getline(lines, row)) {
        // These fixtures contain no quoted commas, so all row types share the same column count.
        if (std::ranges::count(row, ',') != 58)
            return false;
    }
    return true;
}

bool testUsbOperations(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    constexpr std::string_view deviceId = "{6A523E13-8D9C-4CA3-A19C-8B317DFBE275}";
    DiagnosticReader reader(root);
    {
        DiagnosticPublisher publisher(root);
        const auto tick = GetTickCount64();
        DiagnosticSnapshot driving{.mode = DiagnosticMode::usb};
        driving.axes[0] = {.state = DiagnosticAxisState::armed, .value = 0.25f, .appliedAt = tick};

        const auto operation = [&](std::uint64_t began, std::string_view name, DiagnosticUsbLane lane) {
            DiagnosticUsbOperation call{.startedAt = began, .lane = lane, .deviceAxisMask = 1};
            std::ranges::copy(deviceId, call.deviceId.begin());
            std::ranges::copy(name, call.operation.begin());
            return call;
        };
        const auto readerCall = operation(tick - 1000, "GetDeviceState", DiagnosticUsbLane::reader);
        const auto probeCall = operation(tick - 700, "Probe GetObjectInfo axis", DiagnosticUsbLane::probe);
        const auto forceCall = operation(tick - 500, "FFB Stop", DiagnosticUsbLane::feedback);
        const auto shortCall = operation(tick + 60000, "Poll", DiagnosticUsbLane::reader);
        std::array calls{readerCall, probeCall, forceCall, shortCall};
        publisher.publish(driving, 1, {}, true, std::span(calls).first(2));
        const auto driveAt = reader.read().lastDriveAt;
        const DiagnosticSnapshot background{.mode = DiagnosticMode::gameNotForeground};
        publisher.publish(background, 0, {}, false, calls);
        publisher.publish(background, 0, {}, false, calls);
        const auto started = reader.read();
        if (started.usbOperationCount != 3 || started.usbEventCount != 3 ||
            started.usbOperations[0].startedAt != readerCall.startedAt ||
            started.usbOperations[1].lane != DiagnosticUsbLane::probe ||
            started.usbOperations[2].lane != DiagnosticUsbLane::feedback ||
            started.usbEvents[0].mode != DiagnosticMode::usb ||
            started.usbEvents[1].mode != DiagnosticMode::usb ||
            started.usbEvents[2].mode != DiagnosticMode::gameNotForeground ||
            started.usbEvents[2].durationMicros < 500000 ||
            started.lastDriveAt != driveAt || started.lastDrive.axes[0].value != 0.25f)
            return false;
        for (std::size_t i = 0; i <= started.usbEvents.size(); ++i)
            publisher.recordUsbEvent({.sampledAt = tick + i + 10, .kind = DiagnosticUsbEventKind::contextChanged});
        publisher.publish(background, 0, {}, false, calls);
        const auto retained = reader.read();
        if (retained.usbOperationCount != 3 || retained.usbEventCount != retained.usbEvents.size() ||
            std::ranges::any_of(retained.usbEvents, [](const auto& event) {
                return event.kind == DiagnosticUsbEventKind::operationRunning;
            }))
            return false;

        const auto request = reader.requestPerformanceCapture(true);
        const auto acknowledged = publisher.performanceRequest();
        if (!request || !acknowledged || acknowledged->id != request || !acknowledged->enabled)
            return false;
        publisher.performanceRequest();
        calls[0] = operation(tick - 350, "Acquire", DiagnosticUsbLane::reader);
        publisher.publish(background, 0, {}, false, calls);
        publisher.publish(background, 0, {}, false, calls);
        const auto progressed = reader.read();
        if (progressed.usbOperationCount != 3 || progressed.usbOperations[0].operation != calls[0].operation)
            return false;

        // Returning to the foreground does not duplicate an existing call in
        // the CSV. A new effect is recorded with its own observed context.
        publisher.publish(driving, 1, {}, true, calls);
        calls[2] = operation(tick - 300, "FFB SetParameters", DiagnosticUsbLane::feedback);
        publisher.publish(driving, 1, {}, true, calls);
        const auto complete = [&](const DiagnosticUsbOperation& call, DiagnosticMode mode, std::uint64_t duration) {
            publisher.recordUsbEvent({.sampledAt = GetTickCount64(), .durationMicros = duration,
                                      .deviceId = call.deviceId, .operation = call.operation,
                                      .kind = DiagnosticUsbEventKind::deviceSlow, .mode = mode, .lane = call.lane});
        };
        // The stop finished in the background and was reported after focus returned.
        complete(forceCall, DiagnosticMode::gameNotForeground, 750000);
        complete(calls[2], DiagnosticMode::usb, 350000);
        publisher.publish(driving, 1);
        std::array<DiagnosticCaptureRecord, 8> rows;
        const auto stream = reader.drainCapture(rows);
        if (stream.count != 6 || rows[0].usb.operation != calls[0].operation ||
            rows[1].usb.operation != probeCall.operation || rows[2].usb.operation != forceCall.operation ||
            rows[3].usb.operation != calls[2].operation || rows[0].usb.durationMicros < 350000 ||
            rows[2].usb.durationMicros < 500000 || rows[2].usb.lane != DiagnosticUsbLane::feedback ||
            rows[3].usb.lane != DiagnosticUsbLane::feedback || rows[3].usb.mode != DiagnosticMode::usb ||
            std::ranges::any_of(std::span(rows).first(3), [](const auto& row) {
                return row.usb.mode != DiagnosticMode::gameNotForeground;
            }))
            return false;
        std::ostringstream csv;
        writeDiagnosticCaptureRows(csv, std::span(rows).first(stream.count), tick);
        const auto text = csv.str();
        if (text.find(deviceId) != std::string::npos ||
            text.find(",operation_running,\"GetDeviceState\",,") != std::string::npos ||
            text.find(",operation_running,\"Probe GetObjectInfo axis\",,") == std::string::npos ||
            text.find(",operation_running,\"Acquire\",,") == std::string::npos ||
            text.find(",reader,\r\n") == std::string::npos || text.find(",probe,\r\n") == std::string::npos ||
            text.find(",feedback,\r\n") == std::string::npos)
            return false;
        bool recordedStop{}, recordedEffect{}, completedStop{}, completedEffect{};
        std::istringstream csvRows(text);
        for (std::string row; std::getline(csvRows, row);) {
            if (row.find(",operation_running,\"FFB Stop\",,") != std::string::npos)
                recordedStop = row.find(",game_not_foreground,") != std::string::npos && row.ends_with(",feedback,\r");
            if (row.find(",operation_running,\"FFB SetParameters\",,") != std::string::npos)
                recordedEffect = row.find(",usb,") != std::string::npos && row.ends_with(",feedback,\r");
            if (row.find(",device_slow,\"FFB Stop\",0x00000000,750000,") != std::string::npos)
                completedStop = row.find(",game_not_foreground,") != std::string::npos && row.ends_with(",feedback,\r");
            if (row.find(",device_slow,\"FFB SetParameters\",0x00000000,350000,") != std::string::npos)
                completedEffect = row.find(",usb,") != std::string::npos && row.ends_with(",feedback,\r");
        }
        if (!recordedStop || !recordedEffect || !completedStop || !completedEffect)
            return false;

        publisher.publish(background);
        if (reader.read().usbOperationCount != 0)
            return false;
        publisher.publish(background, 0, {}, false, calls);
        if (reader.read().usbOperationCount != 3)
            return false;
    }
    const auto closed = reader.read();
    if (closed.connection != DiagnosticConnection::stale || !closed.lastDriveAt)
        return false;
    DiagnosticPublisher restarted(root);
    restarted.publish({.mode = DiagnosticMode::waitingForDriver});
    const auto next = reader.read();
    return next.connection == DiagnosticConnection::connected && next.usbOperationCount == 0 &&
        next.snapshot.mode == DiagnosticMode::waitingForDriver;
}

bool testPartialDrivingSample(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    DiagnosticPublisher publisher(root);
    DiagnosticReader reader(root);
    DiagnosticSnapshot drive{.mode = DiagnosticMode::usb};
    drive.profile[0] = 'A';
    drive.transmission = {.configured = true, .engagedGear = 2};
    publisher.publish(drive, 1, {}, canRetainDrivingSample(drive.mode, true, false));
    if (reader.read().lastDrive.transmission.engagedGear != 2)
        return false;

    drive.profile[0] = 'B';
    drive.transmission.engagedGear = 3;
    drive.axes[0].state = DiagnosticAxisState::armed;
    drive.axes[0].delivery = DiagnosticDelivery::applied;
    drive.accessories = DiagnosticAccessoryState::deviceUnavailable;
    publisher.publish(drive, 2, {}, canRetainDrivingSample(drive.mode, false, true));
    DiagnosticSnapshot expired{.mode = DiagnosticMode::usb, .accessories = DiagnosticAccessoryState::sampleExpired};
    publisher.publish(expired, 2, {}, canRetainDrivingSample(expired.mode, false, false));
    publisher.publish({.mode = DiagnosticMode::gameNotForeground});
    const auto after = reader.read();
    return after.lastDrive.transmission.engagedGear == 3 && after.lastDrive.profile[0] == 'B' &&
        after.lastDrive.axes[0].delivery == DiagnosticDelivery::applied;
}

bool testStreamingCaptureAfterGameCloses(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    DiagnosticReader reader(root);
    const DiagnosticSnapshot snapshot{.mode = DiagnosticMode::simulation};
    std::array<DiagnosticCaptureRecord, 64> records;
    std::uint64_t firstTick = GetTickCount64();
    DrivingCaptureSample sample{.sampledAt = firstTick, .mode = DiagnosticMode::simulation};
    sample.transmission.engagedGear = 4;
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot);
        reader.read();
        const auto start = reader.requestPerformanceCapture(true);
        if (!start || !publisher.performanceRequest()->enabled)
            return false;
        for (std::size_t i = 0; i < diagnosticCaptureCapacity + records.size(); ++i) {
            publisher.recordDrivingSample(start, sample);
            sample.sampledAt += 25;
            if ((i + 1) % records.size() == 0) {
                publisher.publish(snapshot, 1);
                const auto batch = reader.drainCapture(records);
                if (batch.count != records.size() || batch.dropped || batch.finished || !batch.available ||
                    records.front().driving.sampledAt != firstTick + 25 * (i + 1 - records.size()) ||
                    records.back().driving.sampledAt != firstTick + 25 * i)
                    return false;
            }
        }
        // A blocked reader eventually loses old rows, while newer rows remain available.
        for (std::size_t i = 0; i < diagnosticCaptureCapacity + records.size(); ++i) {
            publisher.recordDrivingSample(start, sample);
            sample.sampledAt += 25;
            if ((i + 1) % records.size() == 0)
                publisher.publish(snapshot, 1);
        }
        publisher.recordDrivingSample(start, sample); // The destructor flushes this last uncommitted row.
    }
    if (reader.read().connection != DiagnosticConnection::stale)
        return false;
    // The next publisher must not discard the preceding capture's tail before its owner drains it.
    DiagnosticPublisher restarted(root);
    restarted.publish(snapshot);
    DiagnosticReader observer(root);
    if (reader.read().connection != DiagnosticConnection::connected ||
        observer.read().connection != DiagnosticConnection::connected || observer.requestPerformanceCapture(true))
        return false;
    const auto first = reader.drainCapture(records);
    if (first.finished || first.dropped != records.size() + 1 || first.count != records.size() || !first.available ||
        records.front().driving.sampledAt != firstTick + 25 * (diagnosticCaptureCapacity + 2 * records.size() + 1))
        return false;
    std::ostringstream csv;
    writeDiagnosticCaptureHeader(csv);
    writeDiagnosticCaptureRows(csv, std::span(records).first(1), firstTick, first.dropped);
    if (csv.str().find(",dropped,65,") == std::string::npos)
        return false;
    for (std::size_t i = records.size(); i < diagnosticCaptureCapacity; i += records.size()) {
        const auto batch = reader.drainCapture(records);
        if (!batch.available || batch.dropped || batch.count != records.size() ||
            batch.finished != (i + records.size() == diagnosticCaptureCapacity))
            return false;
    }
    return records.back().driving.sampledAt == sample.sampledAt && observer.requestPerformanceCapture(true) &&
        !reader.requestPerformanceCapture(false);
}

int run()
{
    using namespace chrome_axis;
    const TemporaryDirectory temp;
    const auto root = temp.path / L"one";
    const auto otherRoot = temp.path / L"two";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(otherRoot);
    DiagnosticReader missing(std::filesystem::path{});
    DiagnosticReader reader(root);
    DiagnosticReader other(otherRoot);
    if (missing.read().connection != DiagnosticConnection::unavailable ||
        reader.read().connection != DiagnosticConnection::disconnected ||
        other.read().connection != DiagnosticConnection::disconnected)
        return 1;
    {
        // An older plugin must remain invisible, even if both halves of its channel exist.
        const auto legacy = installationChannelName(root, L"Local\\ChromeAxis.Diagnostics.v20.");
        const std::unique_ptr<void, decltype(&CloseHandle)> legacyMutex(
            CreateMutexW(nullptr, FALSE, (legacy + L".frame").c_str()), &CloseHandle);
        const std::unique_ptr<void, decltype(&CloseHandle)> legacyMapping(
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16 * 1024 * 1024, legacy.c_str()),
            &CloseHandle);
        if (!legacyMutex || !legacyMapping || reader.read().connection != DiagnosticConnection::disconnected)
            return 34;
    }

    DiagnosticSnapshot snapshot;
    snapshot.mode = DiagnosticMode::usb;
    snapshot.profile[0] = 'A';
    snapshot.axes[0] = {DiagnosticAxisState::armed, -0.75f, GetTickCount64()};
    snapshot.axes[0].inputAvailable = 1;
    snapshot.axes[0].inputValue = 0.0f;
    snapshot.axes[0].requestedAvailable = 1;
    snapshot.axes[0].requestedValue = 0.0f;
    snapshot.axes[0].delivery = DiagnosticDelivery::gameKept;
    snapshot.axes[1].state = DiagnosticAxisState::waitingForNeutral;
    snapshot.axes[1].finalValue = 0.0f;
    snapshot.axes[1].finalAvailable = 1;
    snapshot.axes[2].state = DiagnosticAxisState::armed;
    snapshot.axes[2].inputValue = 0.25f;
    snapshot.axes[2].inputAvailable = 1;
    snapshot.axes[2].requestedValue = 0.125f;
    snapshot.axes[2].requestedAvailable = 1;
    snapshot.axes[2].finalValue = 0.5f;
    snapshot.axes[2].finalAvailable = 1;
    snapshot.axes[2].delivery = DiagnosticDelivery::gameKept;
    snapshot.pedalSampledAt = snapshot.axes[0].appliedAt;
    snapshot.profileLoaded = 1;
    snapshot.accessoryCount = 4;
    snapshot.accessoryArmed = 2;
    snapshot.accessories = DiagnosticAccessoryState::deviceUnavailable;
    snapshot.lastAccessoryKey = VK_SPACE;
    snapshot.accessorySentAt = snapshot.axes[0].appliedAt;
    snapshot.accessoryCommands[0] = {
        .kind = AccessoryKind::Button,
        .role = ShifterRole::Upshift,
        .activation = AccessoryActivation::pulse,
        .index = 7,
        .key = VK_ADD,
        .engaged = 1,
        .state = DiagnosticCommandState::waitingForNeutral,
        .output = DiagnosticCommandOutput::blocked,
        .active = 1,
        .activeAvailable = 1,
    };
    snapshot.accessoryCommands[1] = {
        .kind = AccessoryKind::Axis,
        .index = 2,
        .key = VK_SPACE,
        .rest = 1000,
        .engaged = 500,
        .state = DiagnosticCommandState::ready,
        .output = DiagnosticCommandOutput::held,
        .active = 1,
        .activeAvailable = 1,
        .travelAvailable = 1,
        .travel = 0.75f,
        .lastSentAt = snapshot.accessorySentAt,
    };
    snapshot.accessoryCommands[2] = {
        .kind = AccessoryKind::Axis,
        .index = 4,
        .rest = 300,
        .engaged = 1000,
        .state = DiagnosticCommandState::ready,
        .active = 1,
        .activeAvailable = 1,
        .travelAvailable = 1,
        .travel = 0.5f,
        .handbrakeCeilingPercent = 15,
        .handbrakeRequested = 0.075f,
        .handbrakeFinal = 1.0f,
        .handbrakeAvailable = 1,
    };
    snapshot.accessoryCommands[3] = {
        .kind = AccessoryKind::Button,
        .role = ShifterRole::Reverse,
        .index = 5,
        .engaged = 1,
        .state = DiagnosticCommandState::ready,
        .output = DiagnosticCommandOutput::nativeRefused,
        .active = 1,
        .activeAvailable = 1,
    };
    snapshot.transmission = {true, true, 0, 0, -1, DiagnosticShiftState::stopToChangeDirection};
    snapshot.transmission.shortcutKey = 'J';
    snapshot.transmission.shortcutPressedAt = GetTickCount64();
    snapshot.transmission.shortcutPressCount = 2;
    snapshot.transmission.shortcutSource = DiagnosticShortcutSource::hook;
    snapshot.feedback = DiagnosticFeedbackState::active;
    snapshot.feedbackData = {
        .motionAgeMicros = 16500,
        .submittedAt = snapshot.axes[0].appliedAt,
        .rawRequested = 1.25f,
        .requested = 1.0f,
        .submitted = -0.625f,
        .motionAvailable = 1,
        .requestAvailable = 1,
        .clipped = 1,
        .impactClipped = 1,
        .skidding = 1,
    };
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot, 1);
        const auto received = reader.read();
        const auto& paddle = received.snapshot.accessoryCommands[0];
        const auto& lever = received.snapshot.accessoryCommands[1];
        const auto& handbrake = received.snapshot.accessoryCommands[2];
        if (received.connection != DiagnosticConnection::connected || !received.sampledAt ||
            received.snapshot.mode != snapshot.mode || std::string_view(received.snapshot.profile.data()) != "A" ||
            received.snapshot.axes[0].state != snapshot.axes[0].state ||
            received.snapshot.axes[0].value != snapshot.axes[0].value ||
            received.snapshot.axes[0].appliedAt != snapshot.axes[0].appliedAt ||
            received.snapshot.axes[1].state != snapshot.axes[1].state ||
            received.snapshot.profileLoaded != snapshot.profileLoaded ||
            received.snapshot.accessoryCount != snapshot.accessoryCount ||
            received.snapshot.accessoryArmed != snapshot.accessoryArmed ||
            received.snapshot.accessories != snapshot.accessories ||
            received.snapshot.lastAccessoryKey != snapshot.lastAccessoryKey ||
            received.snapshot.accessorySentAt != snapshot.accessorySentAt ||
            paddle.role != ShifterRole::Upshift || paddle.activation != AccessoryActivation::pulse ||
            paddle.index != 7 || paddle.key != VK_ADD || paddle.state != DiagnosticCommandState::waitingForNeutral ||
            paddle.output != DiagnosticCommandOutput::blocked || !paddle.activeAvailable || !paddle.active ||
            paddle.travelAvailable || paddle.lastSentAt ||
            lever.kind != AccessoryKind::Axis || lever.activation != AccessoryActivation::hold ||
            lever.rest != 1000 || lever.engaged != 500 || lever.output != DiagnosticCommandOutput::held ||
            !lever.travelAvailable || lever.travel != 0.75f || lever.lastSentAt != snapshot.accessorySentAt ||
            handbrake.key || handbrake.lastSentAt || handbrake.handbrakeCeilingPercent != 15 ||
            !handbrake.handbrakeAvailable || handbrake.handbrakeRequested != 0.075f || handbrake.handbrakeFinal != 1.0f ||
            received.snapshot.transmission != snapshot.transmission ||
            received.lastDrive.transmission != snapshot.transmission ||
            received.snapshot.accessoryCommands[3].output != DiagnosticCommandOutput::nativeRefused ||
            received.lastDrive.accessoryCommands[3].output != DiagnosticCommandOutput::nativeRefused ||
            received.snapshot.feedback != snapshot.feedback ||
            received.snapshot.feedbackData.motionAgeMicros != 16500 ||
            received.snapshot.feedbackData.submittedAt != snapshot.feedbackData.submittedAt ||
            received.snapshot.feedbackData.rawRequested != 1.25f ||
            received.snapshot.feedbackData.requested != 1.0f || received.snapshot.feedbackData.submitted != -0.625f ||
            !received.snapshot.feedbackData.motionAvailable || !received.snapshot.feedbackData.requestAvailable ||
            !received.snapshot.feedbackData.clipped || !received.snapshot.feedbackData.impactClipped ||
            received.snapshot.feedbackData.airborne ||
            !received.snapshot.feedbackData.skidding ||
            !received.lastDriveAt || received.lastDrive.axes[0].inputAvailable != 1 ||
            received.lastDrive.axes[0].inputValue != 0.0f || received.lastDrive.axes[1].inputAvailable ||
            received.lastDrive.axes[0].requestedAvailable != 1 || received.lastDrive.axes[0].requestedValue != 0.0f ||
            received.lastDrive.axes[0].delivery != DiagnosticDelivery::gameKept ||
            received.lastDrive.pedalSampledAt != snapshot.pedalSampledAt ||
            !received.lastDrive.axes[1].finalAvailable || received.lastDrive.axes[1].finalValue != 0.0f ||
            received.lastDrive.axes[1].requestedAvailable || !received.lastDrive.axes[2].finalAvailable ||
            received.lastDrive.axes[2].inputValue != 0.25f || received.lastDrive.axes[2].requestedValue != 0.125f ||
            received.lastDrive.axes[2].finalValue != 0.5f ||
            received.eventCount != 1 || received.events[0].mode != DiagnosticMode::usb ||
            other.read().connection != DiagnosticConnection::disconnected)
            return 2;

        const auto duplicate = std::make_unique<DiagnosticPublisher>(root);
        auto different = snapshot;
        different.mode = DiagnosticMode::simulation;
        duplicate->publish(different);
        if (reader.read().snapshot.mode != DiagnosticMode::usb)
            return 3;

        auto focused = snapshot;
        focused.axes[0].inputValue = 0.5f;
        focused.feedbackData.motionAgeMicros = 24000;
        focused.feedbackData.airborne = 1;
        focused.feedbackData.skidding = 0;
        publisher.publish(focused, 1);
        if (reader.read().eventCount != 1)
            return 8;

        auto idle = snapshot;
        idle.mode = DiagnosticMode::gameNotForeground;
        idle.axes = {};
        idle.accessoryCommands = {};
        idle.feedback = DiagnosticFeedbackState::inactive;
        idle.feedbackData = {};
        const auto usbTick = GetTickCount64();
        const DiagnosticUsbEvent slowRead{
            .sampledAt = usbTick, .durationMicros = 505520, .operation = {'P', 'o', 'l', 'l'},
            .kind = DiagnosticUsbEventKind::deviceSlow, .mode = DiagnosticMode::usb, .deviceAxisMask = 0b100,
        };
        publisher.recordUsbEvent(slowRead);
        publisher.publish(idle);
        DiagnosticReader openedAfterAltTab(root);
        const auto retained = openedAfterAltTab.read();
        if (retained.connection != DiagnosticConnection::connected ||
            retained.snapshot.mode != DiagnosticMode::gameNotForeground ||
            retained.lastDrive.mode != DiagnosticMode::usb ||
            retained.snapshot.accessoryCommands[1].lastSentAt ||
            retained.lastDrive.accessoryCommands[1].lastSentAt != snapshot.accessorySentAt ||
            retained.snapshot.accessoryCommands[2].handbrakeAvailable ||
            !retained.lastDrive.accessoryCommands[2].handbrakeAvailable ||
            retained.lastDrive.accessoryCommands[2].handbrakeFinal != 1.0f ||
            retained.lastDrive.axes[0].inputValue != 0.5f || retained.eventCount != 2 ||
            retained.snapshot.feedbackData.motionAvailable || retained.snapshot.feedbackData.submittedAt ||
            retained.lastDrive.feedbackData.motionAgeMicros != 24000 || !retained.lastDrive.feedbackData.airborne ||
            retained.events[1].mode != DiagnosticMode::gameNotForeground ||
            retained.usbEventCount != 1 || retained.usbEvents[0].sampledAt != usbTick ||
            retained.usbEvents[0].durationMicros != slowRead.durationMicros ||
            retained.usbEvents[0].operation != slowRead.operation || retained.usbEvents[0].deviceAxisMask != 0b100)
            return 9;

        for (std::size_t i = 0; i < retained.events.size() + 1; ++i) {
            focused.axes[1].state = i % 2 == 0 ? DiagnosticAxisState::deviceUnavailable : DiagnosticAxisState::armed;
            publisher.publish(focused, 1);
        }
        const auto bounded = reader.read();
        if (bounded.eventCount != bounded.events.size() ||
            bounded.events.front().axes[1] != DiagnosticAxisState::armed ||
            bounded.events.back().axes[1] != DiagnosticAxisState::deviceUnavailable)
            return 10;
        publisher.publish(snapshot, 2);
        const auto nextDrive = reader.read();
        if (nextDrive.eventCount != 1 || nextDrive.lastDrive.axes[1].state != DiagnosticAxisState::waitingForNeutral ||
            nextDrive.usbEventCount != 1 || nextDrive.usbEvents[0].kind != DiagnosticUsbEventKind::deviceSlow)
            return 11;

        for (std::size_t i = 0; i <= retained.usbEvents.size(); ++i) {
            publisher.recordUsbEvent({
                .sampledAt = usbTick + i, .durationMicros = 100001 + i,
                .kind = DiagnosticUsbEventKind::pollExpired, .mode = DiagnosticMode::usb,
            });
        }
        publisher.recordUsbEvent({.sampledAt = usbTick - 1, .kind = DiagnosticUsbEventKind::layoutPending,
                                  .mode = DiagnosticMode::usb});
        auto pending = snapshot;
        pending.axes[0].state = DiagnosticAxisState::temporarilyUnavailable;
        pending.axes[0].inputAvailable = 0;
        pending.accessories = DiagnosticAccessoryState::temporarilyUnavailable;
        pending.feedback = DiagnosticFeedbackState::waitingForMotion;
        pending.feedbackData.motionAvailable = 0;
        pending.feedbackData.requestAvailable = 0;
        publisher.publish(pending, 2);
        publisher.publish(snapshot, 2);
        const auto resumed = reader.read();
        if (resumed.usbEventCount != resumed.usbEvents.size() ||
            resumed.usbEvents.front().durationMicros != 100002 ||
            resumed.usbEvents.back().durationMicros != 100001 + resumed.usbEvents.size())
            return 26;
        if (resumed.eventCount != 3 || resumed.events[1].axes[0] != DiagnosticAxisState::temporarilyUnavailable ||
            resumed.events[1].accessories != DiagnosticAccessoryState::temporarilyUnavailable ||
            resumed.events[1].feedback != DiagnosticFeedbackState::waitingForMotion ||
            resumed.snapshot.axes[0].state != DiagnosticAxisState::armed)
            return 12;

        std::this_thread::sleep_for(1700ms);
        const auto stale = reader.read();
        if (stale.connection != DiagnosticConnection::stale || !stale.lastDriveAt || stale.eventCount != 3)
            return 4;
        publisher.publish(snapshot, 2);
        if (reader.read().connection != DiagnosticConnection::connected)
            return 5;
    }
    if (reader.read().connection != DiagnosticConnection::stale)
        return 6;

    const auto restarted = std::make_unique<DiagnosticPublisher>(root);
    snapshot.mode = DiagnosticMode::waitingForDriver;
    restarted->publish(snapshot);
    const auto restart = reader.read();
    if (restart.connection != DiagnosticConnection::connected ||
        restart.snapshot.mode != DiagnosticMode::waitingForDriver || restart.lastDriveAt ||
        restart.eventCount != 1 || restart.events[0].mode != DiagnosticMode::waitingForDriver || restart.usbEventCount)
        return 7;
    if (!checkPercentiles())
        return 24;
    const auto performance = testPerformanceCapture(otherRoot);
    if (performance != 0)
        return performance;
    if (!testUsbBlockerRecording(otherRoot))
        return 29;
    if (!testPartialDrivingSample(otherRoot))
        return 32;
    if (!testUsbOperations(otherRoot))
        return 33;
    return testStreamingCaptureAfterGameCloses(otherRoot) ? 0 : 25;
}

} // namespace

int main()
{
    const auto result = run();
    if (result)
        std::cerr << "Diagnostic channel regression, check " << result << '\n';
    return result;
}
