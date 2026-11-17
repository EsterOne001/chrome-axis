#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace chrome_axis {

struct ProfileStamp {
    std::uint64_t created{};
    std::uint64_t written{};
    std::uint64_t size{};
    bool operator==(const ProfileStamp&) const = default;
};

inline std::optional<ProfileStamp> profileStamp(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return std::nullopt;
    const auto combine = [](DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; };
    return ProfileStamp{combine(attributes.ftCreationTime.dwHighDateTime, attributes.ftCreationTime.dwLowDateTime),
                        combine(attributes.ftLastWriteTime.dwHighDateTime, attributes.ftLastWriteTime.dwLowDateTime),
                        combine(attributes.nFileSizeHigh, attributes.nFileSizeLow)};
}

struct WatchedProfile {
    using Clock = std::chrono::steady_clock;

    std::optional<ProfileStamp> stamp;
    Clock::time_point retryAt{Clock::time_point::max()};

    bool changed(const std::filesystem::path& path, Clock::time_point now, bool force)
    {
        const auto current = profileStamp(path);
        const bool needsRead = force || stamp != current || now >= retryAt;
        stamp = current;
        return needsRead;
    }

    bool deferLockedRead(DWORD error, bool hasCurrent, Clock::time_point now)
    {
        if (!hasCurrent || !isReadLock(error))
            return false;
        if (!lockedSince_)
            lockedSince_ = now;
        const auto deadline = *lockedSince_ + std::chrono::seconds(1);
        if (now >= deadline)
            return false;
        retryAt = std::min(now + std::chrono::milliseconds(250), deadline);
        return true;
    }

    void readFinished(bool valid, Clock::time_point now, DWORD error = ERROR_SUCCESS)
    {
        lockedSince_.reset();
        retryAt = !valid && (stamp || isReadLock(error)) ? now + std::chrono::seconds(1) : Clock::time_point::max();
    }

private:
    static bool isReadLock(DWORD error) noexcept
    {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
    }

    std::optional<Clock::time_point> lockedSince_;
};

} // namespace chrome_axis
