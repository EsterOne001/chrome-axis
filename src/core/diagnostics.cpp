#include "chrome_axis/diagnostics.hpp"

#include <algorithm>
#include <type_traits>

namespace chrome_axis {

struct DiagnosticFrame {
    LONG64 version{};
    DiagnosticRead report{};
};

namespace {

constexpr LONG64 protocolVersion = 2;
constexpr std::uint64_t heartbeatLifetime = 1500;
static_assert(std::is_trivially_copyable_v<DiagnosticFrame>);

void close(HANDLE handle) noexcept
{
    if (handle)
        CloseHandle(handle);
}

std::wstring channelName(const std::filesystem::path& gameRoot)
{
    if (gameRoot.empty())
        return {};
    const auto file = CreateFileW(gameRoot.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    FILE_ID_INFO id{};
    const bool identified = GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != FALSE;
    BY_HANDLE_FILE_INFORMATION legacy{};
    const bool hasLegacyId = !identified && GetFileInformationByHandle(file, &legacy);
    CloseHandle(file);
    if (!identified && !hasLegacyId)
        return {};

    // Use file identity so different spelling, drive mappings or directory aliases agree.
    std::wstring name = L"Local\\ChromeAxis.Diagnostics.v2.";
    if (identified) {
        name += std::to_wstring(id.VolumeSerialNumber) + L".";
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto byte : id.FileId.Identifier) {
            name += hex[byte >> 4];
            name += hex[byte & 0xf];
        }
    } else {
        name += L"legacy." + std::to_wstring(legacy.dwVolumeSerialNumber) + L"." +
            std::to_wstring(legacy.nFileIndexHigh) + L"." + std::to_wstring(legacy.nFileIndexLow);
    }
    return name;
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

DiagnosticPublisher::DiagnosticPublisher(const std::filesystem::path& gameRoot)
{
    const auto name = channelName(gameRoot);
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

void DiagnosticPublisher::publish(const DiagnosticSnapshot& snapshot, std::uint64_t drivingSession) noexcept
{
    if (!frame_)
        return;
    report_.sampledAt = GetTickCount64();
    report_.snapshot = snapshot;
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
    frame_->report = report_;
    InterlockedExchange64(&frame_->version, protocolVersion);
    ReleaseMutex(mutex_);
}

DiagnosticReader::DiagnosticReader(const std::filesystem::path& gameRoot)
    : channel_(channelName(gameRoot)), mutexName_(channel_ + L".frame")
{
}

DiagnosticReader::~DiagnosticReader()
{
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
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, channel_.c_str());
        if (mapping_)
            frame_ = static_cast<const DiagnosticFrame*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(DiagnosticFrame)));
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
        if (lock == WAIT_OBJECT_0 && frame_->version == protocolVersion && frame_->report.sampledAt) {
            last_ = frame_->report;
            last_.connection = DiagnosticConnection::connected;
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

} // namespace chrome_axis
