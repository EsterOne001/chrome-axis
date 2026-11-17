#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace chrome_axis::detail {

struct SettingsText {
    std::string contents;
    DWORD error{};
    bool invalidSize{};
};

// Keep the Win32 error from the same open or read that failed. A later
// availability check can miss a lock that has already been released.
inline SettingsText readSettingsText(const std::filesystem::path& path, LONGLONG sizeLimit = 16 * 1024)
{
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {.error = GetLastError()};
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size))
        return {.error = GetLastError(), .invalidSize = true};
    if (size.QuadPart < 0 || size.QuadPart > sizeLimit)
        return {.invalidSize = true};

    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytesRead{};
    if (!ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &bytesRead, nullptr))
        return {.error = GetLastError()};
    if (bytesRead != contents.size())
        return {.error = ERROR_HANDLE_EOF};
    return {.contents = std::move(contents)};
}

} // namespace chrome_axis::detail
