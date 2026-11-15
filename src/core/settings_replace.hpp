#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace chrome_axis::detail {

inline std::filesystem::path stagedSettingsPath(const std::filesystem::path& target)
{
    static std::atomic<std::uint64_t> sequence{};
    auto path = target;
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    path += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(id);
    return path;
}

inline bool replaceSettingsFile(const std::filesystem::path& staged, const std::filesystem::path& target)
{
    constexpr std::array<DWORD, 6> retryDelays{5, 10, 20, 40, 80, 100};
    for (std::size_t retry = 0;; ++retry) {
        if (MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const auto error = GetLastError();
        // Windows may report access denied when a reader briefly holds either file.
        if (retry == retryDelays.size()
            || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION))
            return false;
        Sleep(retryDelays[retry]);
    }
}

} // namespace chrome_axis::detail
