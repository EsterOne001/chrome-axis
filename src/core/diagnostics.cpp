#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/configuration.hpp"

#include <algorithm>
#include <type_traits>

namespace chrome_axis {

struct DiagnosticFrame {
    LONG64 version{};
    std::uint64_t publisherId{};
    DiagnosticRead report{};
    PerformanceRequest request{};
    std::uint64_t captureOwner{};
    std::uint64_t requestSerial{};
};

namespace {

constexpr LONG64 protocolVersion = 3;
constexpr std::uint64_t heartbeatLifetime = 1500;
constexpr std::uint64_t performanceLease = 5000;
static_assert(std::is_trivially_copyable_v<DiagnosticFrame>);

void close(HANDLE handle) noexcept
{
    if (handle)
        CloseHandle(handle);
}

constexpr wchar_t diagnosticChannelPrefix[] = L"Local\\ChromeAxis.Diagnostics.v3.";

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
        frame_->publisherId = publisherId_;
    }
    frame_->report = report_;
    InterlockedExchange64(&frame_->version, protocolVersion);
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
        if (lock == WAIT_OBJECT_0 && frame_->version == protocolVersion && frame_->report.sampledAt) {
            if (publisherId_ != frame_->publisherId) {
                publisherId_ = frame_->publisherId;
                captureOwner_ = 0;
            }
            last_ = frame_->report;
            last_.connection = DiagnosticConnection::connected;
            if (captureOwner_) {
                if (frame_->captureOwner == captureOwner_ && frame_->request.enabled &&
                    now < frame_->request.leaseUntil && now - last_.sampledAt <= heartbeatLifetime &&
                    (last_.performance.requestId != frame_->request.id ||
                     last_.performance.state != PerformanceState::complete)) {
                    frame_->request.leaseUntil = now + performanceLease;
                } else {
                    captureOwner_ = 0;
                }
            }
            last_.snapshot.profile.back() = '\0';
            last_.lastDrive.profile.back() = '\0';
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
        }
    }
    ReleaseMutex(mutex_);
    return requestId;
}

} // namespace chrome_axis
