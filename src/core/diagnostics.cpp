#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/configuration.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <string_view>
#include <type_traits>

namespace chrome_axis {

struct SequencedCaptureRecord {
    DiagnosticCaptureRecord row{};
    std::uint64_t sequence{};
};

struct DiagnosticFrame {
    LONG64 version{};
    std::uint64_t publisherId{};
    DiagnosticRead report{};
    PerformanceRequest request{};
    std::uint64_t requestSerial{};
    std::uint64_t captureId{};
    std::uint64_t capturePublisherId{};
    std::uint64_t captureHoldUntil{};
    std::uint64_t captureWritten{};
    std::uint64_t captureSequence{};
    std::uint32_t drivingCaptureSamples{};
    bool captureFinished{};
    std::array<SequencedCaptureRecord, diagnosticCaptureCapacity> capture{};
};

namespace {

constexpr LONG64 protocolVersion = 13;
constexpr std::uint64_t heartbeatLifetime = 1500;
constexpr std::uint64_t performanceLease = 5000;
constexpr std::uint64_t drivingCaptureInterval = 25; // ms, with immediate gear and mode changes.
static_assert(std::is_trivially_copyable_v<DiagnosticFrame>);

void close(HANDLE handle) noexcept
{
    if (handle)
        CloseHandle(handle);
}

constexpr wchar_t diagnosticChannelPrefix[] = L"Local\\ChromeAxis.Diagnostics.v13.";

template <class T, std::size_t Size>
std::string_view csvToken(T value, const std::array<std::string_view, Size>& tokens) noexcept
{
    const auto index = static_cast<std::size_t>(value);
    return index < tokens.size() ? tokens[index] : "unknown";
}

constexpr std::array<std::string_view, 7> modeTokens{
    "waiting_for_driver", "game_not_foreground", "waiting_for_vehicle_input", "usb_disabled", "profile_unavailable",
    "usb", "simulation"
};
constexpr std::array<std::string_view, 14> shiftTokens{
    "automatic", "ready", "unavailable", "outside_range", "stop_to_change_direction", "already_engaged",
    "rejected", "accepted", "neutral_transition", "expired", "recalibration_needed", "selector_unavailable", "pending",
    "clutch_required"
};
constexpr std::array<std::string_view, 10> axisTokens{
    "unassigned", "device_unavailable", "axis_unavailable", "waiting_for_neutral", "armed", "awaiting_profile",
    "inactive", "simulated", "sample_expired", "temporarily_unavailable"
};
constexpr std::array<std::string_view, 11> usbTokens{
    "device_slow", "device_pending", "device_unavailable", "device_recovered", "poll_expired", "poll_gap",
    "commands_waiting", "commands_ready", "context_changed", "menu_paused", "directinput_reopened"
};
constexpr std::array<std::string_view, 11> accessoryTokens{
    "unconfigured", "waiting", "ready", "profile_error", "inactive", "blocked", "device_unavailable",
    "input_unavailable", "sample_expired", "temporarily_unavailable", "axis_conflict"
};
static_assert(modeTokens.size() == static_cast<std::size_t>(DiagnosticMode::simulation) + 1);
static_assert(shiftTokens.size() == static_cast<std::size_t>(DiagnosticShiftState::clutchRequired) + 1);
static_assert(axisTokens.size() == static_cast<std::size_t>(DiagnosticAxisState::temporarilyUnavailable) + 1);
static_assert(usbTokens.size() == static_cast<std::size_t>(DiagnosticUsbEventKind::directInputReopened) + 1);
static_assert(accessoryTokens.size() == static_cast<std::size_t>(DiagnosticAccessoryState::axisConflict) + 1);

constexpr std::size_t drivingCsvColumns = 27;
constexpr std::size_t captureCsvColumns = 44;
constexpr std::size_t usbCsvColumns = 9;

std::uint64_t elapsed(std::uint64_t sampledAt, std::uint64_t startedAt) noexcept
{
    return sampledAt >= startedAt ? sampledAt - startedAt : 0;
}

void emptyCsvColumns(std::ostream& output, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        output.put(',');
}

template <std::size_t Length>
std::string_view csvText(const std::array<char, Length>& value)
{
    const auto end = std::ranges::find(value, '\0');
    return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

void writeCsvText(std::ostream& output, std::string_view text)
{
    if (text.empty())
        return;
    output << '"';
    for (const auto character : text) {
        if (character == '"')
            output << '"';
        output << character;
    }
    output << '"';
}

void recordTransition(DiagnosticRead& report) noexcept
{
    const auto& current = report.snapshot;
    DiagnosticEvent event{report.sampledAt, current.mode, {}, current.accessories, current.feedback};
    for (std::size_t i = 0; i < event.axes.size(); ++i)
        event.axes[i] = current.axes[i].state;
    if (report.eventCount) {
        const auto& previous = report.events[report.eventCount - 1];
        if (previous.mode == event.mode && previous.axes == event.axes
            && previous.accessories == event.accessories && previous.feedback == event.feedback)
            return;
    }
    if (report.eventCount == report.events.size()) {
        std::move(report.events.begin() + 1, report.events.end(), report.events.begin());
        --report.eventCount;
    }
    report.events[report.eventCount++] = event;
}

} // namespace

void PerformanceTiming::add(std::uint64_t micros) noexcept
{
    ++samples;
    totalMicros += micros;
    maxMicros = std::max(maxMicros, micros);
    const auto bucket = std::ranges::lower_bound(bucketLimits, micros);
    ++buckets[static_cast<std::size_t>(bucket - bucketLimits.begin())];
}

std::uint64_t PerformanceTiming::percentile(unsigned int percent) const noexcept
{
    percent = std::clamp(percent, 1u, 100u);
    const auto rank = samples / 100 * percent + ((samples % 100) * percent + 99) / 100;
    std::uint64_t seen{};
    for (std::size_t i = 0; i < bucketLimits.size(); ++i) {
        seen += buckets[i];
        if (seen >= rank)
            return std::min(bucketLimits[i], maxMicros);
    }
    return maxMicros;
}

namespace {

void writeDrivingHeader(std::ostream& output)
{
    output << "elapsed_ms,mode,vehicle,speed_mps,engine_rpm,throttle,brake,clutch_configured,clutch_input,"
              "clutch_assistance,clutch_probe_supported,clutch_test_active,clutch_test_attenuation,"
              "transmission_mode,selector,selected_gear,"
              "requested_gear,engaged_gear,shift_result,shift_delay_us,speed_valid,rpm_valid,throttle_valid,brake_valid,"
              "clutch_input_valid,clutch_attenuation_valid,shift_delay_valid";
}

void writeDrivingRow(std::ostream& output, const DrivingCaptureSample& sample, std::uint64_t startedAt)
{
    const auto valid = [](bool available, float value) { return available && std::isfinite(value); };
    const auto number = [&output](bool available, float value) {
        if (available)
            output << value;
        output << ',';
    };
    const auto gear = [&output](int value) {
        if (value >= -1)
            output << value;
        output << ',';
    };
    output << elapsed(sample.sampledAt, startedAt) << ',' << csvToken(sample.mode, modeTokens) << ',';
    writeCsvText(output, csvText(sample.vehicle));
    output << ',';
    const bool speed = valid(sample.speedAvailable, sample.speedMps);
    const bool rpm = valid(sample.rpmAvailable, sample.rpm);
    const bool throttle = valid(sample.throttleAvailable, sample.throttle);
    const bool brake = valid(sample.brakeAvailable, sample.brake);
    const bool clutchInput = valid(sample.clutch.inputAvailable, sample.clutch.input);
    const bool attenuation = valid(sample.clutch.appliedAvailable, sample.clutch.applied);
    number(speed, sample.speedMps);
    number(rpm, sample.rpm);
    number(throttle, sample.throttle);
    number(brake, sample.brake);
    output << sample.clutch.configured << ',';
    number(clutchInput, sample.clutch.input);
    output << sample.clutch.assistanceEnabled << ',' << sample.clutch.probeSupported << ',' << sample.clutch.probeActive << ',';
    number(attenuation, sample.clutch.applied);
    const auto& transmission = sample.transmission;
    if (transmission.configured) {
        output << (transmission.manual ? "manual" : "auto") << ',';
        if (sample.mode == DiagnosticMode::usb)
            output << (transmission.sequential ? "sequential" : "h_pattern");
        else if (sample.mode == DiagnosticMode::simulation)
            output << "keyboard";
        output << ',';
    } else {
        output << ",,";
    }
    gear(transmission.selectedGear);
    gear(transmission.requestedGear);
    gear(transmission.engagedGear);
    if (transmission.configured)
        output << csvToken(transmission.shift, shiftTokens);
    output << ',';
    if (sample.shiftDelayAvailable)
        output << sample.shiftDelayMicros;
    output << ',' << speed << ',' << rpm << ',' << throttle << ',' << brake << ','
           << clutchInput << ',' << attenuation << ',' << sample.shiftDelayAvailable;
}

void writeCaptureAxes(std::ostream& output, const std::array<DiagnosticAxisState, 3>& axes)
{
    for (const auto axis : axes)
        output << csvToken(axis, axisTokens) << ',';
}

} // namespace

void writeDiagnosticCaptureHeader(std::ostream& output)
{
    writeDrivingHeader(output);
    output << ",record_type,dropped_records,steering_state,throttle_state,brake_state,steering,handbrake,feedback_force,"
              "usb_event,usb_operation,usb_result,usb_duration_us,usb_device_axes,usb_device_accessory_count,"
              "accessory_state,accessory_armed,accessory_count\r\n";
}

void writeDiagnosticCaptureRows(std::ostream& output, std::span<const DiagnosticCaptureRecord> records,
                                std::uint64_t startedAt, std::uint64_t dropped)
{
    output.imbue(std::locale::classic());
    output << std::defaultfloat << std::dec << std::noboolalpha << std::setprecision(6);
    if (dropped) {
        if (!records.empty()) {
            const auto& next = records.front();
            output << elapsed(next.usbIncident ? next.usb.sampledAt : next.driving.sampledAt, startedAt);
        }
        emptyCsvColumns(output, drivingCsvColumns - 1);
        output << ",dropped," << dropped;
        emptyCsvColumns(output, captureCsvColumns - drivingCsvColumns - 2);
        output << "\r\n";
    }
    for (const auto& record : records) {
        if (record.usbIncident) {
            const auto& event = record.usb;
            output << elapsed(event.sampledAt, startedAt) << ',' << csvToken(event.mode, modeTokens);
            emptyCsvColumns(output, drivingCsvColumns - 2);
            output << ",usb_incident,,";
            writeCaptureAxes(output, event.axes);
            emptyCsvColumns(output, 3);
            output << csvToken(event.kind, usbTokens) << ',';
            const auto operation = csvText(event.operation);
            writeCsvText(output, operation);
            output << ',';
            if (!operation.empty())
                output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << event.result
                       << std::dec << std::nouppercase << std::setfill(' ');
            output << ',';
            if (event.durationMicros)
                output << event.durationMicros;
            output << ',' << static_cast<unsigned>(event.deviceAxisMask)
                   << ',' << static_cast<unsigned>(event.deviceAccessoryCount)
                   << ',' << csvToken(event.accessories, accessoryTokens)
                   << ',' << event.accessoryArmed << ',' << event.accessoryCount << "\r\n";
            continue;
        }
        const auto& sample = record.driving;
        if (isDrivingCaptureMode(sample.mode)) {
            writeDrivingRow(output, sample, startedAt);
            output << ",driving,,";
        } else {
            output << elapsed(sample.sampledAt, startedAt) << ',' << csvToken(sample.mode, modeTokens);
            emptyCsvColumns(output, drivingCsvColumns - 2);
            output << ",context,,";
        }
        writeCaptureAxes(output, sample.axes);
        const auto number = [&output](bool available, float value) {
            if (available && std::isfinite(value))
                output << value;
            output.put(',');
        };
        number(sample.steeringAvailable, sample.steering);
        number(sample.handbrakeAvailable, sample.handbrake);
        number(sample.feedbackForceAvailable, sample.feedbackForce);
        emptyCsvColumns(output, usbCsvColumns - 1);
        output << "\r\n";
    }
}

void writeDiagnosticCaptureConnection(std::ostream& output, std::uint64_t startedAt,
                                      std::uint64_t sampledAt, bool connected)
{
    output.imbue(std::locale::classic());
    output << elapsed(sampledAt, startedAt);
    emptyCsvColumns(output, drivingCsvColumns - 1);
    output << (connected ? ",connection_restored" : ",connection_lost");
    emptyCsvColumns(output, captureCsvColumns - drivingCsvColumns - 1);
    output << "\r\n";
}

DiagnosticPublisher::DiagnosticPublisher(const std::filesystem::path& gameRoot)
{
    const auto name = installationChannelName(gameRoot, diagnosticChannelPrefix);
    if (name.empty())
        return;
    const auto ownerName = name + L".publisher";
    const auto mutexName = name + L".frame";
    owner_ = CreateMutexW(nullptr, FALSE, ownerName.c_str());
    if (!owner_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        close(owner_);
        owner_ = nullptr;
        return;
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    publisherId_ = static_cast<std::uint64_t>(counter.QuadPart);
    mutex_ = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex_)
        return;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(DiagnosticFrame), name.c_str());
    if (mapping_)
        frame_ = static_cast<DiagnosticFrame*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(DiagnosticFrame)));
}

DiagnosticPublisher::~DiagnosticPublisher()
{
    if (frame_) {
        const auto lock = WaitForSingleObject(mutex_, 0);
        if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
            if (frame_->version == protocolVersion && frame_->publisherId == publisherId_) {
                InterlockedExchange64(&frame_->version, 0);
                flushCaptureRecords();
                frame_->report.sampledAt = 0;
                if (frame_->capturePublisherId == publisherId_)
                    frame_->captureFinished = true;
                InterlockedExchange64(&frame_->version, protocolVersion);
            }
            ReleaseMutex(mutex_);
        }
        UnmapViewOfFile(frame_);
    }
    close(mapping_);
    close(mutex_);
    close(owner_);
}

void DiagnosticPublisher::publish(const DiagnosticSnapshot& snapshot, std::uint64_t drivingSession,
                                  const PerformanceReport& performance) noexcept
{
    if (!frame_)
        return;
    report_.sampledAt = GetTickCount64();
    report_.snapshot = snapshot;
    report_.performance = performance;
    if (drivingSession) {
        if (drivingSession_ != drivingSession) {
            drivingSession_ = drivingSession;
            report_.eventCount = 0;
        }
        report_.lastDrive = snapshot;
        report_.lastDriveAt = report_.sampledAt;
    }
    recordTransition(report_);
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return;
    // A process can exit during the copy, leaving the mutex abandoned.
    const bool validFrame = frame_->version == protocolVersion;
    InterlockedExchange64(&frame_->version, 0);
    if (!validFrame || frame_->publisherId != publisherId_) {
        if (!validFrame) {
            frame_->captureId = 0;
            frame_->capturePublisherId = 0;
            frame_->captureHoldUntil = 0;
        }
        frame_->request = {};
        frame_->captureFinished = true;
        frame_->publisherId = publisherId_;
    }
    flushCaptureRecords();
    if (frame_->capturePublisherId == publisherId_ && performance.requestId == frame_->request.id &&
        (performance.state == PerformanceState::complete ||
         (!frame_->request.enabled && frame_->request.id && performance.state == PerformanceState::idle))) {
        frame_->captureFinished = true;
    }
    frame_->report = report_;
    InterlockedExchange64(&frame_->version, protocolVersion);
    ReleaseMutex(mutex_);
}

void DiagnosticPublisher::recordUsbEvent(const DiagnosticUsbEvent& event) noexcept
{
    if (!frame_)
        return;
    auto& count = report_.usbEventCount;
    auto& events = report_.usbEvents;
    if (count == events.size()) {
        std::move(events.begin() + 1, events.end(), events.begin());
        --count;
    }
    events[count++] = event;
    if (event.sampledAt)
        queueCaptureRecord({.usb = event, .usbIncident = true});
}

void DiagnosticPublisher::recordDrivingSample(std::uint64_t captureId, const DrivingCaptureSample& sample) noexcept
{
    if (!frame_ || !captureId || captureId != observedRequest_.id || !sample.sampledAt)
        return;
    if (sample.sampledAt < lastCaptureSample_.sampledAt)
        return;
    if (sample.mode == lastCaptureSample_.mode && lastCaptureSample_.sampledAt) {
        if (!isDrivingCaptureMode(sample.mode) ||
            (sample.sampledAt - lastCaptureSample_.sampledAt < drivingCaptureInterval &&
             sample.transmission == lastCaptureSample_.transmission))
            return;
    }
    queueCaptureRecord({.driving = sample});
    lastCaptureSample_ = sample;
}

void DiagnosticPublisher::queueCaptureRecord(const DiagnosticCaptureRecord& row) noexcept
{
    const auto now = GetTickCount64();
    if (!observedRequest_.enabled || now >= observedRequest_.leaseUntil)
        return;
    if (pendingId_ != observedRequest_.id) {
        pendingId_ = observedRequest_.id;
        pendingHead_ = pendingCount_ = 0;
        pendingDropped_ = 0;
    }
    if (pendingCount_ == pending_.size()) {
        pendingHead_ = (pendingHead_ + 1) % pending_.size();
        --pendingCount_;
        ++pendingDropped_;
    }
    pending_[(pendingHead_ + pendingCount_) % pending_.size()] = row;
    ++pendingCount_;
}

void DiagnosticPublisher::flushCaptureRecords() noexcept
{
    if (pendingId_ && frame_->captureId == pendingId_ && frame_->capturePublisherId == publisherId_ &&
        !frame_->captureFinished) {
        frame_->captureSequence += pendingDropped_;
        for (std::size_t i = 0; i < pendingCount_; ++i) {
            const auto& row = pending_[(pendingHead_ + i) % pending_.size()];
            frame_->capture[frame_->captureWritten % frame_->capture.size()] = {row, frame_->captureSequence++};
            ++frame_->captureWritten;
            if (!row.usbIncident && isDrivingCaptureMode(row.driving.mode) &&
                frame_->drivingCaptureSamples < UINT32_MAX)
                ++frame_->drivingCaptureSamples;
        }
    }
    pendingHead_ = pendingCount_ = 0;
    pendingDropped_ = 0;
}

std::optional<PerformanceRequest> DiagnosticPublisher::performanceRequest() noexcept
{
    if (!frame_)
        return std::nullopt;
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return std::nullopt;
    const auto request = frame_->version == protocolVersion && frame_->publisherId == publisherId_
        ? frame_->request : PerformanceRequest{};
    ReleaseMutex(mutex_);
    if (request.id != observedRequest_.id)
        lastCaptureSample_ = {};
    observedRequest_ = request;
    return request;
}

DiagnosticReader::DiagnosticReader(const std::filesystem::path& gameRoot)
    : channel_(installationChannelName(gameRoot, diagnosticChannelPrefix)), mutexName_(channel_ + L".frame"),
      publisherName_(channel_ + L".publisher")
{
}

DiagnosticReader::~DiagnosticReader()
{
    if (captureOwner_)
        requestPerformanceCapture(false);
    if (frame_)
        UnmapViewOfFile(frame_);
    close(mapping_);
    close(mutex_);
}

bool DiagnosticReader::publisherPresent() const noexcept
{
    if (!frame_->report.sampledAt)
        return false;
    if (GetTickCount64() - frame_->report.sampledAt <= heartbeatLifetime)
        return true;
    // A stalled game still owns its named publisher handle. A terminated process does not.
    const auto publisher = OpenMutexW(SYNCHRONIZE, FALSE, publisherName_.c_str());
    close(publisher);
    return publisher != nullptr;
}

DiagnosticRead DiagnosticReader::read() noexcept
{
    if (channel_.empty())
        return {DiagnosticConnection::unavailable};
    if (!frame_) {
        mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName_.c_str());
        if (mutex_)
            mapping_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, channel_.c_str());
        if (mapping_)
            frame_ = static_cast<DiagnosticFrame*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(DiagnosticFrame)));
        if (!frame_) {
            close(mapping_);
            close(mutex_);
            mapping_ = nullptr;
            mutex_ = nullptr;
            return {DiagnosticConnection::disconnected};
        }
    }

    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
        const auto now = GetTickCount64();
        if (frame_->version == protocolVersion) {
            publisherId_ = frame_->publisherId;
            const bool captureMatches = captureId_ && captureId_ == frame_->captureId &&
                capturePublisherId_ == frame_->capturePublisherId;
            if (frame_->report.sampledAt) {
                last_ = frame_->report;
                last_.connection = DiagnosticConnection::connected;
            } else {
                last_.connection = DiagnosticConnection::stale;
                // A normally closed game leaves a completed capture available to its owner.
                if (captureMatches && frame_->report.performance.state == PerformanceState::complete)
                    last_.performance = frame_->report.performance;
            }
            last_.drivingCaptureSamples = captureMatches ? frame_->drivingCaptureSamples : 0;
            if (captureOwner_) {
                if (captureMatches && !frame_->captureFinished && frame_->request.id == captureOwner_ &&
                    frame_->request.enabled && now < frame_->request.leaseUntil && publisherPresent()) {
                    frame_->request.leaseUntil = now + performanceLease;
                    frame_->captureHoldUntil = frame_->request.leaseUntil;
                } else {
                    captureOwner_ = 0;
                }
            }
            last_.snapshot.profile.back() = '\0';
            last_.lastDrive.profile.back() = '\0';
            last_.performance.feedbackProfile.back() = '\0';
            last_.eventCount = std::min(last_.eventCount, static_cast<std::uint32_t>(last_.events.size()));
            last_.usbEventCount = std::min(last_.usbEventCount, static_cast<std::uint32_t>(last_.usbEvents.size()));
        } else {
            last_.connection = DiagnosticConnection::stale;
        }
        ReleaseMutex(mutex_);
    }
    if (!last_.sampledAt || GetTickCount64() - last_.sampledAt > heartbeatLifetime)
        last_.connection = DiagnosticConnection::stale;
    return last_;
}

std::uint64_t DiagnosticReader::requestPerformanceCapture(bool enabled) noexcept
{
    if (!frame_)
        return 0;
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return 0;

    const auto now = GetTickCount64();
    std::uint64_t requestId{};
    if (frame_->version == protocolVersion && frame_->publisherId == publisherId_) {
        const bool connected = frame_->report.sampledAt && now - frame_->report.sampledAt <= heartbeatLifetime;
        const bool ownsCapture = captureOwner_ && frame_->captureId == captureOwner_ &&
            frame_->capturePublisherId == capturePublisherId_ && frame_->request.id == captureOwner_ &&
            frame_->request.enabled && !frame_->captureFinished;
        if (enabled ? (connected && now >= frame_->captureHoldUntil) : ownsCapture) {
            InterlockedExchange64(&frame_->version, 0);
            requestId = ++frame_->requestSerial;
            captureOwner_ = enabled ? requestId : 0;
            frame_->request = {requestId, enabled ? now + performanceLease : 0, enabled};
            frame_->captureHoldUntil = now + performanceLease;
            if (enabled) {
                captureId_ = requestId;
                capturePublisherId_ = publisherId_;
                captureRead_ = nextSequence_ = 0;
                frame_->captureId = captureId_;
                frame_->capturePublisherId = publisherId_;
                frame_->captureWritten = frame_->captureSequence = 0;
                frame_->drivingCaptureSamples = 0;
                frame_->captureFinished = false;
            }
            InterlockedExchange64(&frame_->version, protocolVersion);
        }
    }
    ReleaseMutex(mutex_);
    return requestId;
}

DiagnosticCaptureBatch DiagnosticReader::drainCapture(std::span<DiagnosticCaptureRecord> output) noexcept
{
    DiagnosticCaptureBatch batch;
    if (!frame_ || !captureId_ || output.empty())
        return batch;
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return batch;
    if (frame_->version == protocolVersion && frame_->captureId == captureId_ &&
        frame_->capturePublisherId == capturePublisherId_) {
        batch.available = true;
        const auto write = frame_->captureWritten;
        captureRead_ = std::max(captureRead_, write - std::min<std::uint64_t>(write, frame_->capture.size()));
        while (captureRead_ < write && batch.count < output.size()) {
            const auto& entry = frame_->capture[captureRead_ % frame_->capture.size()];
            // Report a gap before this batch's first row, in recording order.
            if (entry.sequence > nextSequence_) {
                if (batch.count)
                    break;
                batch.dropped = entry.sequence - nextSequence_;
            }
            output[batch.count++] = entry.row;
            nextSequence_ = entry.sequence + 1;
            ++captureRead_;
        }
        if (captureRead_ == write && (frame_->captureFinished || !publisherPresent())) {
            batch.finished = true;
            frame_->captureHoldUntil = 0;
        }
    } else {
        batch.finished = true;
    }
    if (batch.finished)
        captureOwner_ = 0;
    ReleaseMutex(mutex_);
    return batch;
}

} // namespace chrome_axis
