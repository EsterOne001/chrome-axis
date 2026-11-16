#include "chrome_axis/configuration.hpp"

#include "chrome_axis/accessories.hpp"
#include "settings_replace.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <memory>
#include <system_error>

namespace chrome_axis {
namespace {

bool plainFile(DWORD attributes) noexcept
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
        return directory.is_absolute() && plainFile(GetFileAttributesW((directory / usbMarkerFilename).c_str()));
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
        const bool valid = GetFileInformationByHandle(file, &data) && plainFile(data.dwFileAttributes);
        CloseHandle(file);
        if (valid)
            return true;
    }
    error = "Could not enable USB input. Check access to the configuration directory.";
    return false;
}

bool isTransmissionHotkey(unsigned int key) noexcept
{
    return isAccessoryKey(key) && key != 'N' && key != 'R' && (key < '1' || key > '7')
        && key != VK_PRIOR && key != VK_NEXT;
}

std::optional<unsigned int> readTransmissionHotkey(const std::filesystem::path& profileRoot)
{
    if (!profileRoot.is_absolute())
        return std::nullopt;
    const auto path = profileRoot / transmissionHotkeyFilename;
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
    BY_HANDLE_FILE_INFORMATION information{};
    std::array<char, 64> bytes{};
    DWORD count{};
    if (!GetFileInformationByHandle(handle, &information) || !plainFile(information.dwFileAttributes)
        || !ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count == bytes.size())
        return std::nullopt;

    std::string_view text(bytes.data(), count);
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    const auto line = text.find('\n');
    if (line == text.npos || (text.substr(0, line) != "version=1" && text.substr(0, line) != "version=1\r"))
        return std::nullopt;
    text.remove_prefix(line + 1);
    if (!text.starts_with("key="))
        return std::nullopt;
    text.remove_prefix(4);
    if (text.ends_with('\n'))
        text.remove_suffix(1);
    if (text.ends_with('\r'))
        text.remove_suffix(1);

    unsigned int key{};
    const auto end = text.data() + text.size();
    const auto number = std::from_chars(text.data(), end, key);
    if (number.ec != std::errc{} || number.ptr != end || !isTransmissionHotkey(key))
        return std::nullopt;
    return key;
}

unsigned int loadTransmissionHotkey(const std::filesystem::path& profileRoot)
{
    return readTransmissionHotkey(profileRoot).value_or(defaultTransmissionHotkey);
}

bool saveTransmissionHotkey(const std::filesystem::path& profileRoot, unsigned int key, std::string& error)
{
    error.clear();
    if (!profileRoot.is_absolute()) {
        error = "The Chrome Axis profile directory is unavailable.";
        return false;
    }
    if (!isTransmissionHotkey(key)) {
        error = "Invalid or reserved transmission hotkey.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(profileRoot, directoryError);
    if (directoryError) {
        error = "Could not create the configuration directory.";
        return false;
    }
    const auto path = profileRoot / transmissionHotkeyFilename;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (!plainFile(attributes) && (attributes != INVALID_FILE_ATTRIBUTES || !missing(GetLastError()))) {
        error = "The transmission hotkey settings file is unavailable.";
        return false;
    }

    const auto temporary = detail::stagedSettingsPath(path);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << "version=1\nkey=" << std::to_string(key) << '\n';
    file.close();
    if (file && detail::replaceSettingsFile(temporary, path))
        return true;

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "Could not save the transmission hotkey.";
    return false;
}

} // namespace chrome_axis
