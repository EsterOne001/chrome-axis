#include "chrome_axis/diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
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
        performance.usbIntervalOverruns = 4;
        performance.feedbackIntervalOverruns = 5;
        performance.peakContinuousForce = 1.125f;
        performance.referenceFeedbackGain = 130;
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
            .shortcutPressedAt = sample.sampledAt, .hasRequestedMode = true, .requestedManual = true,
            .shortcutAvailable = true, .gearboxSampledAt = sample.sampledAt,
        };
        sample.clutch = {0.7f, 0.4f, true, true, true, false, true, true};
        publisher.recordDrivingSample(start + 1, sample);
        publisher.recordDrivingSample(start, sample);
        sample.sampledAt += 5;
        sample.transmission.shortcutPressedAt = sample.sampledAt;
        sample.transmission.gearboxSampledAt = sample.sampledAt - 2;
        publisher.recordDrivingSample(start, sample); // Timestamps alone do not force a row.
        sample.sampledAt += 5;
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
            live.performance.usbIntervalOverruns != 4 || live.performance.feedbackIntervalOverruns != 5 ||
            live.performance.peakContinuousForce != 1.125f || live.performance.referenceFeedbackGain != 130 ||
            live.drivingCaptureSamples || owner.read().drivingCaptureSamples != 3)
            return 15;

        std::array<DiagnosticCaptureRecord, 7> records;
        const auto firstBatch = owner.drainCapture(std::span(records).first(2));
        if (!firstBatch.available || firstBatch.finished || firstBatch.dropped || firstBatch.count != 2 ||
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
        const auto tail = owner.drainCapture(std::span(records).subspan(2));
        if (!tail.available || !tail.finished || tail.count != 5 || tail.dropped ||
            records.front().driving.transmission.shift != DiagnosticShiftState::pending ||
            records[1].driving.transmission.shortcutPressedAt != performance.startedAt + 5 ||
            records[1].driving.shiftDelayMicros != 9900 || records[2].driving.mode != DiagnosticMode::usbDisabled ||
            !records[2].driving.speedAvailable || records[2].driving.speedMps != 12.5f ||
            records[3].driving.mode != DiagnosticMode::waitingForVehicleInput ||
            records[4].driving.mode != DiagnosticMode::waitingForVehicleInput ||
            records[4].driving.transmission.shortcutKey != 'L' ||
            records[4].driving.transmission.configured || records[5].driving.transmission.engagedGear != 2 ||
            !records[6].usbIncident || records[6].usb.durationMicros != 52100 ||
            owner.drainCapture(records).count || observer.read().drivingCaptureSamples ||
            observer.drainCapture(records).available)
            return 22;
        std::ostringstream csv;
        writeDiagnosticCaptureHeader(csv);
        writeDiagnosticCaptureRows(csv, std::span(records).first(2), performance.startedAt);
        writeDiagnosticCaptureRows(csv, std::span(records).subspan(2), performance.startedAt);
        writeDiagnosticCaptureConnection(csv, performance.startedAt, performance.startedAt + 100, false);
        writeDiagnosticCaptureConnection(csv, performance.startedAt, performance.startedAt + 200, true);
        const auto text = csv.str();
        constexpr std::string_view pendingRow =
            "\r\n0,usb,\"Car, \"\"A\"\"\",12.5,,0.5,0,1,0.7,1,0,1,0.4,manual,sequential,3,3,2,pending,,1,0,1,1,1,1,0,driving,";
        constexpr std::string_view acceptedRow =
            "\r\n10,usb,\"Car, \"\"A\"\"\",12.5,,0.5,0,1,0.7,1,0,1,0.4,manual,sequential,3,3,3,accepted,9900,1,0,1,1,1,1,1,driving,";
        if (!text.starts_with("elapsed_ms,mode,vehicle,speed_mps,engine_rpm,") ||
            text.find(pendingRow) == std::string::npos || text.find(acceptedRow) == std::string::npos ||
            text.find(",119,1,0,manual,0\r\n") == std::string::npos ||
            text.find(",119,1,5,manual,7\r\n") == std::string::npos ||
            text.find("\r\n30,usb_disabled,") == std::string::npos ||
            text.find("\r\n50,waiting_for_vehicle_input,") == std::string::npos ||
            text.find("\r\n70,waiting_for_vehicle_input,,,,,,,,,,,,manual,,2,2,2,accepted,,,,,,,,,context,") == std::string::npos ||
            text.find(",76,0,,manual,\r\n") == std::string::npos ||
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
    first.axes[1] = {.state = DiagnosticAxisState::armed, .value = 0.6f, .appliedAt = tick};
    publisher.publish(first, 1);
    const auto initial = reader.read();
    if (!initial.lastDriveAt || initial.lastDrive.transmission != first.transmission)
        return false;

    DiagnosticSnapshot waiting{.mode = DiagnosticMode::waitingForVehicleInput};
    waiting.transmission.shortcutKey = 'L';
    publisher.publish(waiting, 1);
    const auto interrupted = reader.read();
    if (interrupted.snapshot.mode != DiagnosticMode::waitingForVehicleInput ||
        interrupted.snapshot.transmission.shortcutKey != 'L' || interrupted.lastDriveAt != initial.lastDriveAt ||
        interrupted.lastDrive.transmission != first.transmission)
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
        background.lastDriveAt != initial.lastDriveAt || background.events[2].axes[1] != DiagnosticAxisState::sampleExpired ||
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
                                            .transmission = first.transmission}},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[0], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[1], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[2], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[3], .usbIncident = true},
        DiagnosticCaptureRecord{.usb = resumed.usbEvents[4], .usbIncident = true},
        DiagnosticCaptureRecord{.driving = {.sampledAt = tick + 50, .mode = waiting.mode,
                                            .transmission = waiting.transmission}},
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
        "accessory:pov:1:119:input_unavailable|handbrake:axis:4:0:sample_expired,,,,,\r\n";
    if (text.find(blockersCell) == std::string::npos || text.find(",76,0,,,\r\n") == std::string::npos ||
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
        if (std::ranges::count(row, ',') != 50)
            return false;
    }
    return true;
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

        DiagnosticPublisher duplicate(root);
        auto different = snapshot;
        different.mode = DiagnosticMode::simulation;
        duplicate.publish(different);
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

    DiagnosticPublisher restarted(root);
    snapshot.mode = DiagnosticMode::waitingForDriver;
    restarted.publish(snapshot);
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
