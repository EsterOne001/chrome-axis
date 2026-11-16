#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/configuration.hpp"
#include "settings_replace.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <ostream>
#include <string_view>
#include <type_traits>

namespace chrome_axis {

struct DiagnosticFrame {
    LONG64 version{};
    std::uint64_t publisherId{};
    DiagnosticRead report{};
    PerformanceRequest request{};
    std::uint64_t captureOwner{};
    std::uint64_t requestSerial{};
    std::uint64_t drivingCaptureId{};
    std::uint32_t drivingCaptureCount{};
    std::array<DrivingCaptureSample, maxDrivingCaptureSamples> drivingCapture{};
};

namespace {

constexpr LONG64 protocolVersion = 11;
constexpr std::uint64_t heartbeatLifetime = 1500;
constexpr std::uint64_t performanceLease = 5000;
constexpr std::uint64_t drivingCaptureInterval = 25; // ms, with immediate gear and mode changes.
static_assert(std::is_trivially_copyable_v<DiagnosticFrame>);

void close(HANDLE handle) noexcept
{
    if (handle)
        CloseHandle(handle);
}

constexpr wchar_t diagnosticChannelPrefix[] = L"Local\\ChromeAxis.Diagnostics.v11.";

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
static_assert(modeTokens.size() == static_cast<std::size_t>(DiagnosticMode::simulation) + 1);
static_assert(shiftTokens.size() == static_cast<std::size_t>(DiagnosticShiftState::clutchRequired) + 1);

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

void writeDrivingCaptureCsv(std::ostream& output, std::span<const DrivingCaptureSample> samples)
{
    output.imbue(std::locale::classic());
    output << std::setprecision(6)
           << "elapsed_ms,mode,vehicle,speed_mps,engine_rpm,throttle,brake,clutch_configured,clutch_input,"
              "clutch_assistance,clutch_probe_supported,clutch_test_active,clutch_test_attenuation,"
              "transmission_mode,selector,selected_gear,"
              "requested_gear,engaged_gear,shift_result,shift_delay_us,speed_valid,rpm_valid,throttle_valid,brake_valid,"
              "clutch_input_valid,clutch_attenuation_valid,shift_delay_valid\r\n";
    const auto startedAt = samples.empty() ? 0 : samples.front().sampledAt;
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
    for (const auto& sample : samples) {
        output << (sample.sampledAt >= startedAt ? sample.sampledAt - startedAt : 0) << ','
               << csvToken(sample.mode, modeTokens) << ',';
        const auto vehicleEnd = std::ranges::find(sample.vehicle, '\0');
        writeCsvText(output, {sample.vehicle.data(), static_cast<std::size_t>(vehicleEnd - sample.vehicle.begin())});
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
               << clutchInput << ',' << attenuation << ',' << sample.shiftDelayAvailable << "\r\n";
    }
}

bool saveDrivingCaptureCsv(const std::filesystem::path& path, std::span<const DrivingCaptureSample> samples)
{
    const auto temporary = detail::stagedSettingsPath(path);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << "\xEF\xBB\xBF"; // Preserve UTF-8 vehicle names in spreadsheet tools.
    writeDrivingCaptureCsv(file, samples);
    file.close();
    if (file && detail::replaceSettingsFile(temporary, path))
        return true;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
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
            frame_->report.sampledAt = 0;
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
    InterlockedExchange64(&frame_->version, 0);
    if (lock == WAIT_ABANDONED || frame_->publisherId != publisherId_) {
        if (frame_->publisherId != publisherId_)
            frame_->requestSerial = 0;
        frame_->request = {};
        frame_->captureOwner = 0;
        frame_->drivingCaptureId = 0;
        frame_->drivingCaptureCount = 0;
        frame_->publisherId = publisherId_;
    }
    frame_->report = report_;
    InterlockedExchange64(&frame_->version, protocolVersion);
    ReleaseMutex(mutex_);
}

void DiagnosticPublisher::recordDrivingSample(std::uint64_t captureId, const DrivingCaptureSample& sample) noexcept
{
    if (!frame_ || !captureId || !sample.sampledAt || !isDrivingCaptureMode(sample.mode))
        return;
    if (lastCaptureId_ == captureId) {
        if (sample.sampledAt < lastCaptureSample_.sampledAt)
            return;
        if (sample.sampledAt - lastCaptureSample_.sampledAt < drivingCaptureInterval &&
            sample.mode == lastCaptureSample_.mode && sample.transmission == lastCaptureSample_.transmission)
            return;
    }
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return;
    if (lock == WAIT_OBJECT_0 && frame_->version == protocolVersion && frame_->publisherId == publisherId_ &&
        frame_->drivingCaptureId == captureId && frame_->request.enabled &&
        GetTickCount64() < frame_->request.leaseUntil && frame_->drivingCaptureCount < maxDrivingCaptureSamples) {
        frame_->drivingCapture[frame_->drivingCaptureCount++] = sample;
        lastCaptureId_ = captureId;
        lastCaptureSample_ = sample;
    }
    ReleaseMutex(mutex_);
}

std::optional<PerformanceRequest> DiagnosticPublisher::performanceRequest() const noexcept
{
    if (!frame_)
        return std::nullopt;
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock == WAIT_ABANDONED) {
        frame_->request = {};
        frame_->captureOwner = 0;
        frame_->drivingCaptureId = 0;
        frame_->drivingCaptureCount = 0;
        ReleaseMutex(mutex_);
        return PerformanceRequest{};
    }
    if (lock != WAIT_OBJECT_0)
        return std::nullopt;
    const auto request = frame_->publisherId == publisherId_ ? frame_->request : PerformanceRequest{};
    ReleaseMutex(mutex_);
    return request;
}

DiagnosticReader::DiagnosticReader(const std::filesystem::path& gameRoot)
    : channel_(installationChannelName(gameRoot, diagnosticChannelPrefix)), mutexName_(channel_ + L".frame")
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
        if (lock == WAIT_OBJECT_0 && frame_->version == protocolVersion) {
            if (publisherId_ != frame_->publisherId) {
                publisherId_ = frame_->publisherId;
                captureOwner_ = 0;
                captureId_ = 0;
            }
            const bool captureMatches = captureId_ && captureId_ == frame_->drivingCaptureId &&
                frame_->report.performance.requestId == frame_->request.id;
            if (frame_->report.sampledAt) {
                last_ = frame_->report;
                last_.connection = DiagnosticConnection::connected;
            } else {
                last_.connection = DiagnosticConnection::stale;
                // A normally closed game leaves a completed capture available to its owner.
                if (captureMatches && frame_->report.performance.state == PerformanceState::complete)
                    last_.performance = frame_->report.performance;
            }
            last_.drivingCaptureSamples = captureMatches ? static_cast<std::uint32_t>(
                std::min<std::size_t>(frame_->drivingCaptureCount, maxDrivingCaptureSamples)) : 0;
            if (captureOwner_) {
                if (frame_->captureOwner == captureOwner_ && frame_->request.enabled &&
                    frame_->report.sampledAt && now < frame_->request.leaseUntil &&
                    now - frame_->report.sampledAt <= heartbeatLifetime &&
                    (last_.performance.requestId != frame_->request.id ||
                     last_.performance.state != PerformanceState::complete)) {
                    frame_->request.leaseUntil = now + performanceLease;
                } else {
                    captureOwner_ = 0;
                }
            }
            last_.snapshot.profile.back() = '\0';
            last_.lastDrive.profile.back() = '\0';
            last_.performance.feedbackProfile.back() = '\0';
            last_.eventCount = std::min(last_.eventCount, static_cast<std::uint32_t>(last_.events.size()));
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
    if (lock == WAIT_ABANDONED) {
        frame_->request = {};
        frame_->captureOwner = 0;
        frame_->drivingCaptureId = 0;
        frame_->drivingCaptureCount = 0;
        ReleaseMutex(mutex_);
        return 0;
    }
    if (lock != WAIT_OBJECT_0)
        return 0;

    const auto now = GetTickCount64();
    std::uint64_t requestId{};
    const bool connected = frame_->version == protocolVersion && frame_->publisherId == publisherId_ &&
        frame_->report.sampledAt && now - frame_->report.sampledAt <= heartbeatLifetime;
    if (connected) {
        const auto& current = frame_->request;
        const bool ownsCapture = captureOwner_ && frame_->captureOwner == captureOwner_;
        const bool reserved = current.enabled && now < current.leaseUntil &&
            (frame_->report.performance.requestId != current.id ||
             frame_->report.performance.state != PerformanceState::complete);
        if (enabled ? (!reserved || ownsCapture) : ownsCapture) {
            requestId = ++frame_->requestSerial;
            captureOwner_ = enabled ? requestId : 0;
            frame_->captureOwner = captureOwner_;
            frame_->request = {requestId, enabled ? now + performanceLease : 0, enabled};
            if (enabled) {
                captureId_ = requestId;
                frame_->drivingCaptureId = captureId_;
                frame_->drivingCaptureCount = 0;
            }
        }
    }
    ReleaseMutex(mutex_);
    return requestId;
}

std::vector<DrivingCaptureSample> DiagnosticReader::readDrivingCapture()
{
    std::vector<DrivingCaptureSample> samples;
    if (!frame_ || !captureId_ || !last_.drivingCaptureSamples || last_.performance.state != PerformanceState::complete)
        return samples;
    samples.reserve(last_.drivingCaptureSamples);
    const auto lock = WaitForSingleObject(mutex_, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
        return samples;
    if (lock == WAIT_OBJECT_0 && frame_->version == protocolVersion && frame_->publisherId == publisherId_ &&
        frame_->drivingCaptureId == captureId_ && frame_->report.performance.state == PerformanceState::complete &&
        frame_->report.performance.requestId == frame_->request.id &&
        frame_->drivingCaptureCount == last_.drivingCaptureSamples) {
        const auto begin = frame_->drivingCapture.begin();
        samples.assign(begin, begin + frame_->drivingCaptureCount);
    }
    ReleaseMutex(mutex_);
    return samples;
}

} // namespace chrome_axis
