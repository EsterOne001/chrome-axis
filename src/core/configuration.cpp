#include "chrome_axis/configuration.hpp"

#include <memory>
#include <system_error>

namespace chrome_axis {
namespace {

bool validMarker(DWORD attributes) noexcept
{
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool missing(DWORD code) noexcept
{
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

} // namespace

std::filesystem::path modulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size())
        return {};

    path.resize(size);
    return path;
}

std::filesystem::path profileDirectory(const std::filesystem::path& moduleFile)
{
    if (!moduleFile.is_absolute() || !moduleFile.has_filename())
        return {};

    return moduleFile.parent_path() / L"Profil";
}

std::filesystem::path installedGameRoot(const std::filesystem::path& pluginDirectory)
{
    const auto plugins = pluginDirectory.parent_path();
    const auto red4ext = plugins.parent_path();
    const auto matches = [](const std::filesystem::path& path, const wchar_t* name) {
        return CompareStringOrdinal(path.filename().c_str(), -1, name, -1, TRUE) == CSTR_EQUAL;
    };
    if (!pluginDirectory.is_absolute() || !matches(pluginDirectory, L"chrome_axis")
        || !matches(plugins, L"plugins") || !matches(red4ext, L"red4ext"))
        return {};

    const auto gameRoot = red4ext.parent_path();
    std::error_code error;
    return std::filesystem::is_regular_file(gameRoot / L"bin" / L"x64" / L"Cyberpunk2077.exe", error)
        ? gameRoot : std::filesystem::path{};
}

PluginFiles pluginFiles(const std::filesystem::path& directory)
{
    if (!directory.is_absolute())
        return {};
    std::error_code error;
    return {
        std::filesystem::is_regular_file(directory / L"ChromeAxis.dll", error),
        std::filesystem::is_regular_file(directory / L"scripts" / L"driving_context.reds", error)
    };
}

bool isInstalledPluginDirectory(const std::filesystem::path& directory)
{
    return !installedGameRoot(directory).empty() && pluginFiles(directory).complete();
}

std::filesystem::path calibrationProfileDirectory(const std::filesystem::path& appFile)
{
    const auto local = profileDirectory(appFile);
    if (local.empty() || !installedGameRoot(appFile.parent_path()).empty())
        return local;

    std::wstring buffer(32768, L'\0');
    if (!GetVolumePathNameW(appFile.c_str(), buffer.data(), static_cast<DWORD>(buffer.size())))
        return local;
    const std::filesystem::path volume(buffer.c_str());

    DWORD size = static_cast<DWORD>(buffer.size());
    const HANDLE handle = FindFirstFileNameW(appFile.c_str(), 0, &size, buffer.data());
    if (handle == INVALID_HANDLE_VALUE)
        return local;
    const std::unique_ptr<void, decltype(&FindClose)> links(handle, &FindClose);

    std::filesystem::path deployed;
    do {
        const auto link = volume / std::filesystem::path(buffer.c_str()).relative_path();
        const auto directory = link.parent_path();
        std::error_code error;
        if (CompareStringOrdinal(link.filename().c_str(), -1, L"ChromeAxis.exe", -1, TRUE) == CSTR_EQUAL
            && !installedGameRoot(directory).empty() && std::filesystem::equivalent(appFile, link, error)) {
            // A staging file can be linked to more than one installation.
            if (!deployed.empty())
                return local;
            deployed = profileDirectory(link);
        }
        size = static_cast<DWORD>(buffer.size());
    } while (FindNextFileNameW(links.get(), &size, buffer.data()));

    return GetLastError() == ERROR_HANDLE_EOF && !deployed.empty() ? deployed : local;
}

std::wstring installationChannelName(const std::filesystem::path& gameRoot, std::wstring_view prefix)
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
    std::wstring name(prefix);
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

bool isTemporaryProfileDirectory(const std::filesystem::path& directory)
{
    if (!directory.is_absolute())
        return false;

    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, buffer);
    if (length == 0 || length > MAX_PATH)
        return false;
    auto temporary = std::filesystem::path(buffer).lexically_normal();
    if (!temporary.has_filename())
        temporary = temporary.parent_path();
    if (!temporary.is_absolute() || temporary == temporary.root_path())
        return false;

    const auto normalized = directory.lexically_normal();
    auto part = normalized.begin();
    for (const auto& ancestor : temporary) {
        if (part == normalized.end() || CompareStringOrdinal(part->c_str(), -1, ancestor.c_str(), -1, TRUE) != CSTR_EQUAL)
            return false;
        ++part;
    }
    return true;
}

bool usbEnabled(const std::filesystem::path& directory) noexcept
{
    try {
        return directory.is_absolute() && validMarker(GetFileAttributesW((directory / usbMarkerFilename).c_str()));
    } catch (...) {
        return false;
    }
}

bool setUsbEnabled(const std::filesystem::path& directory, bool enabled, std::string& error)
{
    error.clear();
    if (!directory.is_absolute()) {
        error = "The Chrome Axis profile directory is unavailable.";
        return false;
    }

    const auto marker = directory / usbMarkerFilename;
    if (!enabled) {
        if (DeleteFileW(marker.c_str()) || missing(GetLastError()))
            return true;
        error = "Could not disable USB input. Check access to the configuration directory.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        error = "Could not create the configuration directory.";
        return false;
    }
    const HANDLE file = CreateFileW(marker.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION data{};
        const bool valid = GetFileInformationByHandle(file, &data) && validMarker(data.dwFileAttributes);
        CloseHandle(file);
        if (valid)
            return true;
    }
    error = "Could not enable USB input. Check access to the configuration directory.";
    return false;
}

} // namespace chrome_axis
