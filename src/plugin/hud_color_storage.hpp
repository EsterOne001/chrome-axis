#pragma once

#include "../core/settings_replace.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace chrome_axis {
namespace detail {

inline constexpr DWORD maxHudColorBytes = 4 * 1024 * 1024;

struct HudColorHandleCloser {
    void operator()(HANDLE file) const noexcept { CloseHandle(file); }
};

using HudColorHandle = std::unique_ptr<void, HudColorHandleCloser>;

inline HudColorHandle hudColorHandle(HANDLE file) noexcept {
    return HudColorHandle{file == INVALID_HANDLE_VALUE ? nullptr : file};
}

inline std::optional<std::string> readHudColorBytes(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > maxHudColorBytes
        || !SetFilePointerEx(file, LARGE_INTEGER{}, nullptr, FILE_BEGIN))
        return std::nullopt;
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD count{};
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) || count != text.size())
        return std::nullopt;
    return text;
}

inline bool sameHudColorText(std::string_view bytes, std::string_view previous) noexcept {
    if (bytes == previous)
        return true;
    // RedFileSystem reads text with the Windows CRT, which converts CRLF to LF.
    for (std::size_t position = 0; position < bytes.size(); ++position) {
        if (bytes[position] == '\r' && position + 1 < bytes.size() && bytes[position + 1] == '\n')
            ++position;
        if (previous.empty() || previous.front() != bytes[position])
            return false;
        previous.remove_prefix(1);
    }
    return previous.empty();
}

} // namespace detail

// The caller supplies JSON serialized by RedData and the exact text it merged.
// Keep the old file available until the new one has been written and verified.
inline bool commitHudColors(const std::filesystem::path& gameRoot, std::string_view json,
                            std::string_view previousJson, bool existed) {
    if (gameRoot.empty() || json.empty() || json.size() > detail::maxHudColorBytes
        || previousJson.size() > detail::maxHudColorBytes || (existed && previousJson.empty()))
        return false;

    const auto directory = gameRoot / L"r6" / L"storages" / L"ChromeAxisHUD";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto target = directory / L"colors.json";
    const auto staged = detail::stagedSettingsPath(target);
    auto output = detail::hudColorHandle(CreateFileW(staged.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                                     nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output)
        return false;
    struct RemoveStage {
        const std::filesystem::path& path;
        detail::HudColorHandle& output;
        ~RemoveStage() {
            output.reset();
            DeleteFileW(path.c_str());
        }
    } cleanup{staged, output};

    DWORD count{};
    if (!WriteFile(output.get(), json.data(), static_cast<DWORD>(json.size()), &count, nullptr)
        || count != json.size() || !FlushFileBuffers(output.get()))
        return false;
    const auto saved = detail::readHudColorBytes(output.get());
    if (!saved || *saved != json)
        return false;
    output.reset();

    if (!existed)
        return MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;

    // Check for an intervening write just before publication. Windows requires the
    // target handle to be closed before the replacement, even with FILE_SHARE_DELETE.
    auto current = detail::hudColorHandle(CreateFileW(target.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!current)
        return false;
    const auto observed = detail::readHudColorBytes(current.get());
    if (!observed || !detail::sameHudColorText(*observed, previousJson))
        return false;
    current.reset();
    return MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace chrome_axis
