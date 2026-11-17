#include "chrome_axis/presets.hpp"

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace chrome_axis {
namespace {

constexpr wchar_t presetsFolder[] = L"Profils";
constexpr wchar_t selectionFile[] = L"active_profile.txt";
constexpr wchar_t backupMarker[] = L"chrome_axis_backup.txt";
constexpr wchar_t profileLockFilename[] = L".chrome_axis_profile_backup.lock";
constexpr wchar_t profileRestoreJournal[] = L".chrome_axis_profile_restore.pending";
constexpr wchar_t originalFiles[] = L"Fichiers-originaux";
constexpr std::string_view backupVersion = "Chrome Axis profile backup 1";
constexpr std::string_view reassignmentVersion = "Chrome Axis device reassignment 1 ";
constexpr std::wstring_view reassignmentPrefix = L"Avant-reassociation-";
constexpr std::wstring_view restorePrefix = L"Avant-restauration-";
constexpr std::size_t maxNameLength = 64;
constexpr std::array settingsFiles{profileFilename, accessoryFilename, feedbackFilename};
constexpr std::array globalFiles{selectionFile, profileLanguageFilename, transmissionHotkeyFilename, usbMarkerFilename};
// Apply the USB marker last. Move it away first when taking out the previous settings.
constexpr std::array restoreEntries{profileFilename, accessoryFilename, feedbackFilename, presetsFolder,
                                    selectionFile, profileLanguageFilename, transmissionHotkeyFilename,
                                    usbMarkerFilename};
std::atomic<unsigned long> temporaryId{};

bool missing(DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool sharingLocked(DWORD error) noexcept
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

bool plainDirectory(DWORD attributes) noexcept
{
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool plainFile(DWORD attributes) noexcept
{
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

int compareNames(std::wstring_view left, std::wstring_view right) noexcept
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE);
}

bool validName(std::wstring_view name) noexcept
{
    if (name.empty() || name.size() > maxNameLength || name.front() == L'.' || name.front() == L' '
        || name.back() == L'.' || name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != name.npos
        || std::ranges::any_of(name, [](wchar_t c) { return c < 32; }))
        return false;

    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ')
        base.remove_suffix(1);
    for (const std::wstring_view device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (compareNames(base, device) == CSTR_EQUAL)
            return false;
    }
    const auto port = base.substr(0, 3);
    return base.size() != 4 || (compareNames(port, L"COM") != CSTR_EQUAL && compareNames(port, L"LPT") != CSTR_EQUAL)
        || std::wstring_view(L"123456789¹²³").find(base.back()) == std::wstring_view::npos;
}

std::optional<std::string> encodeName(std::wstring_view name)
{
    if (name.empty())
        return std::string{};
    const auto length = static_cast<int>(name.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes == 0)
        return std::nullopt;
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, result.data(), bytes, nullptr, nullptr))
        return std::nullopt;
    return result;
}

std::optional<std::string> readSmallTextFile(const std::filesystem::path& path, bool* locked = nullptr)
{
    const auto readError = [locked](DWORD error) -> std::optional<std::string> {
        if (locked)
            *locked = sharingLocked(error);
        return std::nullopt;
    };
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return missing(error) ? std::optional(std::string{}) : readError(error);
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
    BY_HANDLE_FILE_INFORMATION information{};
    std::array<char, maxNameLength * 4 + 8> bytes{};
    DWORD count{};
    if (!GetFileInformationByHandle(handle, &information))
        return readError(GetLastError());
    if (!plainFile(information.dwFileAttributes))
        return std::nullopt;
    if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
        return readError(GetLastError());
    if (count >= bytes.size())
        return std::nullopt;

    std::string_view text(bytes.data(), count);
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    if (text.ends_with('\n'))
        text.remove_suffix(1);
    if (text.ends_with('\r'))
        text.remove_suffix(1);
    return std::string(text);
}

std::optional<std::wstring> readSelection(const std::filesystem::path& path, bool* locked = nullptr)
{
    const auto contents = readSmallTextFile(path, locked);
    if (!contents)
        return std::nullopt;
    if (contents->empty())
        return std::wstring{};
    const auto length = static_cast<int>(contents->size());
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, contents->data(), length, nullptr, 0);
    if (units <= 0 || units > static_cast<int>(maxNameLength))
        return std::nullopt;
    std::wstring name(static_cast<std::size_t>(units), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, contents->data(), length, name.data(), units) || !validName(name))
        return std::nullopt;
    return name;
}

std::filesystem::path presetDirectory(const std::filesystem::path& root, std::wstring_view name)
{
    if (name.empty())
        return root;
    const auto parent = root / presetsFolder;
    const auto child = parent / name;
    if (plainDirectory(GetFileAttributesW(parent.c_str())) && plainDirectory(GetFileAttributesW(child.c_str())))
        return child;
    return {};
}

std::filesystem::path temporaryPath(const std::filesystem::path& parent)
{
    const auto id = temporaryId.fetch_add(1, std::memory_order_relaxed);
    return parent / (L".chrome_axis_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(id));
}

bool reportWindowsFailure(std::uint32_t* output, DWORD code) noexcept
{
    if (output)
        *output = code;
    return false;
}

bool writeTextFile(const std::filesystem::path& target, std::string_view contents,
                   std::uint32_t* windowsError = nullptr)
{
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        if (!missing(code))
            return reportWindowsFailure(windowsError, code);
    } else if (!plainFile(attributes)) {
        return false;
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto temporary = temporaryPath(target.parent_path());
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_EXISTS)
                continue;
            return reportWindowsFailure(windowsError, code);
        }

        auto text = std::string(contents);
        text += '\n';
        const auto length = static_cast<DWORD>(text.size());
        DWORD written{};
        const bool writeSucceeded = WriteFile(file, text.data(), length, &written, nullptr);
        const bool writtenAll = writeSucceeded && written == length;
        auto code = writeSucceeded ? ERROR_SUCCESS : GetLastError();
        const bool complete = writtenAll && FlushFileBuffers(file);
        if (writtenAll && !complete)
            code = GetLastError();
        CloseHandle(file);
        if (complete) {
            if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            code = GetLastError();
        }
        DeleteFileW(temporary.c_str());
        return reportWindowsFailure(windowsError, code);
    }
    return reportWindowsFailure(windowsError, ERROR_FILE_EXISTS);
}

std::filesystem::path createTemporaryDirectory(const std::filesystem::path& parent, std::uint32_t* windowsError = nullptr)
{
    DWORD code{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto path = temporaryPath(parent);
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS)
            break;
    }
    if (windowsError)
        *windowsError = code;
    return {};
}

bool copySavedFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                   std::uint32_t* windowsError = nullptr)
{
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        return missing(code) || reportWindowsFailure(windowsError, code);
    }
    if (!plainFile(attributes))
        return false;
    if (!CopyFileExW(source.c_str(), destination.c_str(), nullptr, nullptr, nullptr,
                     COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK))
        return reportWindowsFailure(windowsError, GetLastError());
    const DWORD copiedAttributes = GetFileAttributesW(destination.c_str());
    if (copiedAttributes == INVALID_FILE_ATTRIBUTES)
        return reportWindowsFailure(windowsError, GetLastError());
    if (!plainFile(copiedAttributes))
        return false;
    // An exported or named copy needs to remain editable even if the source is read-only.
    const auto writable = copiedAttributes & ~FILE_ATTRIBUTE_READONLY;
    return copiedAttributes == writable ||
        SetFileAttributesW(destination.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL) ||
        reportWindowsFailure(windowsError, GetLastError());
}

bool copySettings(const std::filesystem::path& source, const std::filesystem::path& destination,
                  std::uint32_t* windowsError = nullptr)
{
    return std::ranges::all_of(settingsFiles, [&](const auto filename) {
        return copySavedFile(source / filename, destination / filename, windowsError);
    });
}

void removeTemporaryDirectory(const std::filesystem::path& path)
{
    for (const auto filename : settingsFiles) {
        const auto file = path / filename;
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (plainFile(attributes)) {
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0)
                SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
            DeleteFileW(file.c_str());
        }
    }
    RemoveDirectoryW(path.c_str());
}

bool validLanguage(std::string_view language)
{
    return language == "auto" || language == "fr" || language == "en";
}

bool writeProfileLanguageFile(const std::filesystem::path& root, std::string_view language,
                              std::uint32_t* windowsError = nullptr)
{
    if (!root.is_absolute() || !validLanguage(language))
        return false;
    const auto attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return reportWindowsFailure(windowsError, GetLastError());
    return plainDirectory(attributes) && writeTextFile(root / profileLanguageFilename, language, windowsError);
}

bool samePathPart(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return compareNames(left.native(), right.native()) == CSTR_EQUAL;
}

std::filesystem::path extendedPath(const std::filesystem::path& path)
{
    if (!path.is_absolute())
        return {};
    auto normalized = path.lexically_normal().make_preferred().native();
    if (normalized.starts_with(LR"(\\?\)"))
        return normalized;
    if (normalized.starts_with(LR"(\\)"))
        return LR"(\\?\UNC\)" + normalized.substr(2);
    return LR"(\\?\)" + normalized;
}

bool insideOrSame(const std::filesystem::path& path, const std::filesystem::path& ancestor)
{
    auto part = path.begin();
    for (const auto& component : ancestor) {
        if (component.empty())
            break;
        if (part == path.end() || !samePathPart(*part, component))
            return false;
        ++part;
    }
    return true;
}

bool validBackupLocation(const std::filesystem::path& root, const std::filesystem::path& other)
{
    if (!root.is_absolute() || !other.is_absolute())
        return false;
    std::error_code error;
    const auto live = std::filesystem::weakly_canonical(root, error);
    if (error)
        return false;
    const auto backup = std::filesystem::weakly_canonical(other, error);
    if (error || insideOrSame(live, backup))
        return false;
    const auto backups = live / profileBackupsFolder;
    return !insideOrSame(backup, live)
        || (insideOrSame(backup, backups) && !insideOrSame(backups, backup));
}

bool optionalEntry(const std::filesystem::path& path, bool directory, bool& exists)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    exists = attributes != INVALID_FILE_ATTRIBUTES;
    return exists ? (directory ? plainDirectory(attributes) : plainFile(attributes)) : missing(GetLastError());
}

bool hasRestoreJournal(const std::filesystem::path& root)
{
    const auto attributes = GetFileAttributesW((root / profileRestoreJournal).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES || !missing(GetLastError());
}

bool clearRestoreJournal(const std::filesystem::path& root, std::uint32_t* windowsError)
{
    if (DeleteFileW((root / profileRestoreJournal).c_str()))
        return true;
    const auto error = GetLastError();
    return missing(error) || reportWindowsFailure(windowsError, error);
}

bool ensureDirectory(const std::filesystem::path& path, std::uint32_t* windowsError)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
        return true;
    const auto code = GetLastError();
    return plainDirectory(GetFileAttributesW(path.c_str())) || reportWindowsFailure(windowsError, code);
}

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory()
    {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

struct ProfileLock {
    explicit ProfileLock(const std::filesystem::path& parent)
    {
        const auto path = parent / profileLockFilename;
        handle = CreateFileW(path.c_str(), DELETE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            failure = GetLastError();
    }

    HANDLE handle;
    DWORD failure{};

    ~ProfileLock()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    explicit operator bool() const noexcept { return handle != INVALID_HANDLE_VALUE; }
};

ProfileBackupError copySnapshot(const std::filesystem::path& source, const std::filesystem::path& destination,
                                bool allowEmpty, std::uint32_t* windowsError)
{
    bool foundProfile = false;
    const auto copyFile = [&](const wchar_t* filename, bool profileSetting) {
        bool exists{};
        if (!optionalEntry(source / filename, false, exists))
            return ProfileBackupError::invalidBackup;
        if (exists && !copySavedFile(source / filename, destination / filename, windowsError))
            return ProfileBackupError::ioError;
        foundProfile |= exists && profileSetting;
        return ProfileBackupError::none;
    };
    for (const auto filename : settingsFiles) {
        if (const auto error = copyFile(filename, true); error != ProfileBackupError::none)
            return error;
    }
    for (const auto filename : globalFiles) {
        if (const auto error = copyFile(filename, false); error != ProfileBackupError::none)
            return error;
    }

    const auto profiles = source / presetsFolder;
    bool hasProfiles{};
    if (!optionalEntry(profiles, true, hasProfiles))
        return ProfileBackupError::invalidBackup;
    if (hasProfiles) {
        const auto target = destination / presetsFolder;
        if (!ensureDirectory(target, windowsError))
            return ProfileBackupError::ioError;
        std::error_code error;
        std::filesystem::directory_iterator it(profiles, error);
        for (; !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
            const auto name = it->path().filename();
            if (name.native().starts_with(L".chrome_axis_"))
                continue;
            const auto attributes = GetFileAttributesW(it->path().c_str());
            if (plainFile(attributes))
                continue;
            if (!plainDirectory(attributes) || !validName(name.native()))
                return ProfileBackupError::invalidBackup;
            const auto copied = target / name;
            if (!CreateDirectoryW(copied.c_str(), nullptr)) {
                reportWindowsFailure(windowsError, GetLastError());
                return ProfileBackupError::ioError;
            }
            if (!copySettings(it->path(), copied, windowsError))
                return ProfileBackupError::ioError;
            foundProfile = foundProfile || std::ranges::any_of(settingsFiles, [&](const auto filename) {
                return plainFile(GetFileAttributesW((copied / filename).c_str()));
            });
        }
        if (error) {
            if (windowsError && error.category() == std::system_category())
                *windowsError = static_cast<std::uint32_t>(error.value());
            return ProfileBackupError::ioError;
        }
    }

    bool marked{};
    if (!optionalEntry(source / backupMarker, false, marked)
        || (marked && readSmallTextFile(source / backupMarker) != backupVersion))
        return ProfileBackupError::invalidBackup;
    if (foundProfile || marked || allowEmpty)
        return ProfileBackupError::none;
    // A manually copied, explicitly selected named profile can have no settings yet.
    const auto selected = readSelection(destination / selectionFile);
    return selected && !selected->empty() && !presetDirectory(destination, *selected).empty()
        ? ProfileBackupError::none : ProfileBackupError::invalidBackup;
}

bool validSavedSettings(const std::filesystem::path& path, bool* locked = nullptr,
                        std::optional<unsigned int> requiredFiles = std::nullopt)
{
    std::string error;
    const auto validate = [&](std::size_t index, const auto& load) {
        const auto file = path / settingsFiles[index];
        bool exists{};
        if (!optionalEntry(file, false, exists)
            || (requiredFiles && exists != ((*requiredFiles & (1u << index)) != 0)))
            return false;
        DWORD readError{};
        const bool valid = !exists || load(file, error, &readError).has_value();
        if (locked && sharingLocked(readError))
            *locked = true;
        return valid;
    };
    return validate(0, loadProfile) && validate(1, loadAccessoryProfile) && validate(2, loadFeedbackSettings);
}

bool validSnapshot(const std::filesystem::path& path, bool* locked = nullptr)
{
    const auto selected = readSelection(path / selectionFile, locked);
    bool hasLanguage{}, hasHotkey{};
    if (!selected || presetDirectory(path, *selected).empty() || !validSavedSettings(path, locked)
        || !optionalEntry(path / profileLanguageFilename, false, hasLanguage)
        || (hasLanguage && !readProfileLanguage(path, locked))
        || !optionalEntry(path / transmissionHotkeyFilename, false, hasHotkey))
        return false;
    // Read errors on the marker and global preferences must not cause the pending recovery to be discarded.
    if (locked && (readSmallTextFile(path / backupMarker, locked) != backupVersion || !readSmallTextFile(path / usbMarkerFilename, locked)
        || (hasHotkey && !readSmallTextFile(path / transmissionHotkeyFilename, locked))))
        return false;
    if (hasHotkey && !readTransmissionHotkey(path))
        return false;
    return std::ranges::all_of(listPresets(path), [&](const auto& name) {
        return validSavedSettings(path / presetsFolder / name, locked);
    });
}

std::optional<unsigned int> reassignmentFiles(const std::filesystem::path& backup, bool* locked = nullptr)
{
    const auto marker = readSmallTextFile(backup / backupMarker, locked);
    if (!marker || marker->size() != reassignmentVersion.size() + 1 || !marker->starts_with(reassignmentVersion)
        || marker->back() < '1' || marker->back() > '7')
        return std::nullopt;
    return static_cast<unsigned int>(marker->back() - '0');
}

std::filesystem::path reassignmentTarget(const std::filesystem::path& root, const std::filesystem::path& backup,
                                         bool* locked = nullptr)
{
    if (!plainFile(GetFileAttributesW((backup / selectionFile).c_str())))
        return {};
    const auto selected = readSelection(backup / selectionFile, locked);
    return selected ? presetDirectory(root, *selected) : std::filesystem::path{};
}

bool activeSnapshotAxisConflict(const std::filesystem::path& path)
{
    const auto selected = activePreset(path);
    if (selected.directory.empty())
        return false;
    std::string error;
    const auto axes = loadProfile(selected.directory / profileFilename, error);
    const auto accessories = loadAccessoryProfile(selected.directory / accessoryFilename, error);
    return axes && accessories && hasAnalogAccessoryAxisConflict(*axes, *accessories);
}

std::filesystem::path createRecoveryDirectory(const std::filesystem::path& parent, std::uint32_t* windowsError,
                                              std::wstring_view prefix = restorePrefix)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto name = std::format(L"{}{:04}{:02}{:02}-{:02}{:02}{:02}", prefix, time.wYear, time.wMonth,
                                  time.wDay, time.wHour, time.wMinute, time.wSecond);
    DWORD code{};
    for (int index = 0; index < 100; ++index) {
        const auto path = parent / (index == 0 ? name : name + L"-" + std::to_wstring(index + 1));
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS)
            break;
    }
    if (windowsError)
        *windowsError = code;
    return {};
}

bool beginRecovery(const std::filesystem::path& root, const std::filesystem::path& backup,
                   std::uint32_t* windowsError = nullptr)
{
    const auto name = encodeName(backup.filename().native());
    return name && writeTextFile(root / profileRestoreJournal, *name, windowsError);
}

struct RestoreEntry {
    bool present{};
    bool incoming{};
    bool saved{};
    bool applied{};
};

bool moveEntry(const std::filesystem::path& from, const std::filesystem::path& to, const wchar_t* filename,
               std::uint32_t* windowsError = nullptr)
{
    const auto source = from / filename;
    const auto destination = to / filename;
    return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) ||
        reportWindowsFailure(windowsError, GetLastError());
}

struct FileHandleCloser {
    void operator()(HANDLE file) const noexcept { CloseHandle(file); }
};

struct SettingsSnapshot {
    std::unique_ptr<void, FileHandleCloser> file;
    BY_HANDLE_FILE_INFORMATION info{};
    std::string bytes;
    bool present{};
};

DeviceReplacementError readSettings(const std::filesystem::path& path, SettingsSnapshot& snapshot)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return missing(GetLastError()) ? DeviceReplacementError::none : DeviceReplacementError::ioError;
    snapshot.file.reset(file);
    snapshot.present = true;
    if (!GetFileInformationByHandle(file, &snapshot.info))
        return DeviceReplacementError::ioError;
    const auto& info = snapshot.info;
    if (!plainFile(info.dwFileAttributes) || info.nFileSizeHigh != 0 || info.nFileSizeLow > 16 * 1024)
        return DeviceReplacementError::invalidProfile;
    snapshot.bytes.resize(info.nFileSizeLow);
    DWORD length{};
    if (!ReadFile(file, snapshot.bytes.data(), info.nFileSizeLow, &length, nullptr) || length != info.nFileSizeLow)
        return DeviceReplacementError::ioError;
    return DeviceReplacementError::none;
}

ProfileFileRevision revisionFrom(const BY_HANDLE_FILE_INFORMATION& info)
{
    const auto combine = [](DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; };
    return {info.dwVolumeSerialNumber, combine(info.nFileIndexHigh, info.nFileIndexLow),
            combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
            combine(info.nFileSizeHigh, info.nFileSizeLow), true};
}

std::optional<ProfileFileRevision> readRevision(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return missing(GetLastError()) ? std::optional{ProfileFileRevision{}} : std::nullopt;
    const std::unique_ptr<void, FileHandleCloser> current(file);
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) && plainFile(info.dwFileAttributes)
        ? std::optional{revisionFrom(info)} : std::nullopt;
}

bool matchesSettings(const std::filesystem::path& path, const SettingsSnapshot& expected)
{
    return readRevision(path) == (expected.present ? revisionFrom(expected.info) : ProfileFileRevision{});
}

bool writeSettings(const std::filesystem::path& path, std::string_view contents)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const std::unique_ptr<void, FileHandleCloser> file(handle);
    const auto length = static_cast<DWORD>(contents.size());
    DWORD written{};
    return WriteFile(handle, contents.data(), length, &written, nullptr) && length == written && FlushFileBuffers(handle);
}

std::string_view trimSettings(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto start = value.find_first_not_of(whitespace);
    if (start == value.npos)
        return {};
    return value.substr(start, value.find_last_not_of(whitespace) - start + 1);
}

bool replaceDeviceTokens(std::string& bytes, bool accessories, std::string_view oldId, std::string_view newId)
{
    bool changed = false;
    for (std::size_t start = 0; start < bytes.size();) {
        const auto newline = bytes.find('\n', start);
        const auto end = newline == bytes.npos ? bytes.size() : newline;
        const auto line = trimSettings(std::string_view(bytes).substr(start, end - start));
        const auto equals = line.find('=');
        if (equals != line.npos) {
            const auto name = trimSettings(line.substr(0, equals));
            const bool deviceEntry = accessories ? name == "binding" || line.starts_with(progressiveHandbrakePrefix)
                    || line.starts_with(nativeShifterPrefix) || line.starts_with(nativeSequentialPrefix)
                    || line.starts_with(clutchPrefix)
                : name == "steering.device" || name == "throttle.device" || name == "brake.device";
            if (deviceEntry) {
                auto value = line.substr(equals + 1);
                if (accessories)
                    value = value.substr(0, value.find(','));
                value = trimSettings(value);
                if (value == oldId) {
                    // Both IDs are GUIDs of the same length, so all other bytes keep their offsets.
                    bytes.replace(static_cast<std::size_t>(value.data() - bytes.data()), oldId.size(), newId);
                    changed = true;
                }
            }
        }
        start = end + 1;
    }
    return changed;
}

bool conflictingInputs(const std::optional<Profile>& axes, const std::optional<AccessoryProfile>& accessories,
                       std::string_view oldId, std::string_view newId)
{
    if (!accessories)
        return false;
    const auto merges = [&](std::string_view left, std::string_view right) {
        return (left == oldId && right == newId) || (left == newId && right == oldId);
    };
    for (std::size_t index = 0; index < accessories->size(); ++index) {
        const auto& binding = (*accessories)[index];
        for (std::size_t other = index + 1; other < accessories->size(); ++other) {
            const auto& second = (*accessories)[other];
            if (binding.kind != second.kind || binding.index != second.index || !merges(binding.deviceId, second.deviceId))
                continue;
            if (binding.kind == AccessoryKind::Button || (binding.kind == AccessoryKind::Pov && binding.engaged == second.engaged))
                return true;
            if (binding.kind == AccessoryKind::Axis) {
                const auto delta = static_cast<double>(binding.engaged) - binding.rest;
                const auto secondDelta = static_cast<double>(second.engaged) - second.rest;
                // A shared lever can trigger opposite directions from the same physical rest point.
                if (binding.rest != second.rest || delta * secondDelta >= 0.0)
                    return true;
            }
        }
        if (axes && binding.kind == AccessoryKind::Axis && !binding.clutchMode && !binding.handbrakeCeilingPercent) {
            for (const auto* axis : {&axes->steering, &axes->throttle, &axes->brake}) {
                if (axis->axis == binding.index && merges(axis->deviceId, binding.deviceId))
                    return true;
            }
        }
    }
    if (!axes)
        return false;

    auto movedAxes = *axes;
    for (auto* axis : {&movedAxes.steering, &movedAxes.throttle, &movedAxes.brake}) {
        if (axis->deviceId == oldId)
            axis->deviceId = newId;
    }
    auto movedBindings = *accessories;
    for (auto& binding : movedBindings) {
        if (binding.deviceId == oldId)
            binding.deviceId = newId;
    }
    return introducesAnalogAccessoryAxisConflict(&*axes, *accessories, movedAxes, movedBindings);
}

} // namespace

bool profileRestorePending(const std::filesystem::path& root)
{
    const auto path = extendedPath(root);
    return !path.empty() && hasRestoreJournal(path);
}

ProfileRestoreRecovery readInterruptedProfileRestore(const std::filesystem::path& root)
{
    const auto path = extendedPath(root);
    if (path.empty() || !hasRestoreJournal(path))
        return {};
    ProfileRestoreRecovery recovery{.pending = true};
    const auto name = readSelection(path / profileRestoreJournal, &recovery.backupLocked);
    if (!name)
        return recovery;
    recovery.deviceReassignment = name->starts_with(reassignmentPrefix);
    const auto backups = path / profileBackupsFolder;
    if ((!recovery.deviceReassignment && !name->starts_with(restorePrefix)) || !plainDirectory(GetFileAttributesW(backups.c_str()))
        || !plainDirectory(GetFileAttributesW((backups / *name).c_str())))
        return recovery;
    recovery.backup = root / profileBackupsFolder / *name;
    if (recovery.deviceReassignment) {
        const auto target = reassignmentTarget(path, backups / *name, &recovery.backupLocked);
        if (!target.empty()) {
            const auto files = reassignmentFiles(backups / *name, &recovery.backupLocked);
            recovery.backupUsable = files && validSavedSettings(backups / *name, &recovery.backupLocked, files);
        }
    } else {
        recovery.backupUsable = validSnapshot(backups / *name, &recovery.backupLocked);
    }
    return recovery;
}

ProfilePreset activePreset(const std::filesystem::path& root)
{
    return readActivePreset(root).value_or(ProfilePreset{});
}

std::optional<ProfilePreset> readActivePreset(const std::filesystem::path& root)
{
    if (!root.is_absolute())
        return ProfilePreset{};
    bool locked{};
    const auto selected = readSelection(root / selectionFile, &locked);
    if (locked)
        return std::nullopt;
    if (!selected)
        return ProfilePreset{};
    return ProfilePreset{*selected, presetDirectory(root, *selected)};
}

std::optional<ProfileFileRevision> profileFileRevision(const std::filesystem::path& path)
{
    const auto extended = extendedPath(path);
    return extended.empty() ? std::nullopt : readRevision(extended);
}

PresetSaveResult savePresetSettings(const std::filesystem::path& root, const ProfilePreset& expected,
                                    std::span<const ProfileFileCheck> checks, const std::function<bool()>& save)
{
    const auto extendedRoot = extendedPath(root);
    if (extendedRoot.empty() || expected.directory.empty())
        return PresetSaveResult::changed;
    const ProfileLock lock(extendedRoot.parent_path());
    if (!lock || hasRestoreJournal(extendedRoot))
        return PresetSaveResult::ioError;
    const auto current = activePreset(root);
    if (current.directory.empty() || current.name != expected.name || !samePathPart(current.directory, expected.directory))
        return PresetSaveResult::changed;
    for (const auto& check : checks) {
        const auto revision = profileFileRevision(expected.directory / check.filename);
        if (!revision || !check.revision)
            return PresetSaveResult::ioError;
        if (revision != check.revision)
            return PresetSaveResult::changed;
    }
    return save() ? PresetSaveResult::saved : PresetSaveResult::ioError;
}

bool saveInstallationSettings(const std::filesystem::path& root, const std::function<bool()>& save)
{
    const auto extendedRoot = extendedPath(root);
    if (extendedRoot.empty())
        return false;
    const ProfileLock lock(extendedRoot.parent_path());
    return lock && !hasRestoreJournal(extendedRoot) && save();
}

std::vector<std::wstring> listPresets(const std::filesystem::path& root)
{
    const auto parent = root / presetsFolder;
    if (!root.is_absolute() || !plainDirectory(GetFileAttributesW(parent.c_str())))
        return {};

    std::vector<std::wstring> names;
    std::error_code error;
    std::filesystem::directory_iterator it(parent, error);
    for (; !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
        auto name = it->path().filename().wstring();
        if (validName(name) && plainDirectory(GetFileAttributesW(it->path().c_str())))
            names.push_back(std::move(name));
    }
    std::ranges::sort(names, [](const auto& left, const auto& right) { return compareNames(left, right) == CSTR_LESS_THAN; });
    return names;
}

namespace {

bool writePresetSelection(const std::filesystem::path& root, std::wstring_view name, std::string& error)
{
    error.clear();
    if (!root.is_absolute() || (!name.empty() && !validName(name)) || presetDirectory(root, name).empty()) {
        error = "Le profil sélectionné est introuvable.";
        return false;
    }
    const auto encoded = encodeName(name);
    std::error_code ioError;
    std::filesystem::create_directories(root, ioError);
    if (!encoded || ioError || !writeTextFile(root / selectionFile, *encoded)) {
        error = "Impossible d’enregistrer la sélection dans le dossier Profil.";
        return false;
    }
    return true;
}

} // namespace

bool selectPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error)
{
    const auto target = root.is_absolute() && (name.empty() || validName(name))
        ? presetDirectory(root, name) : std::filesystem::path{};
    if (target.empty()) {
        error = "Le profil sélectionné est introuvable.";
        return false;
    }
    std::error_code ioError;
    std::filesystem::create_directories(root, ioError);
    const auto extendedRoot = extendedPath(root);
    const ProfileLock lock(extendedRoot.parent_path());
    if (ioError || !lock || hasRestoreJournal(extendedRoot)) {
        error = "Impossible d’enregistrer la sélection dans le dossier Profil.";
        return false;
    }
    if (usbEnabled(root)) {
        std::string details;
        DWORD axesError{}, accessoriesError{};
        const auto axes = loadProfile(target / profileFilename, details, &axesError);
        const auto accessories = loadAccessoryProfile(target / accessoryFilename, details, &accessoriesError);
        if (sharingLocked(axesError) || sharingLocked(accessoriesError)) {
            error = "The selected profile is temporarily locked. Try again after it is unlocked.";
            return false;
        }
        if (axes && accessories && hasAnalogAccessoryAxisConflict(*axes, *accessories)) {
            error = "Disable in-game USB before selecting and repairing this profile's conflicting axes.";
            return false;
        }
    }
    return writePresetSelection(root, name, error);
}

bool createPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error)
{
    error.clear();
    if (!validName(name) || !encodeName(name)) {
        error = "Saisissez un nom de profil valide (64 caractères maximum).";
        return false;
    }
    const auto extendedRoot = extendedPath(root);
    std::error_code ioError;
    if (!extendedRoot.empty())
        std::filesystem::create_directories(root, ioError);
    if (extendedRoot.empty() || ioError) {
        error = "Impossible de créer le dossier Profils.";
        return false;
    }
    const ProfileLock lock(extendedRoot.parent_path());
    if (!lock || hasRestoreJournal(extendedRoot)) {
        error = "Impossible de copier les réglages vers le nouveau profil.";
        return false;
    }
    const auto source = activePreset(root);
    if (source.directory.empty()) {
        error = "Sélectionnez un profil existant avant d’en créer une copie.";
        return false;
    }

    const auto parent = root / presetsFolder;
    if (!CreateDirectoryW(parent.c_str(), nullptr) && !plainDirectory(GetFileAttributesW(parent.c_str()))) {
        error = "Impossible de créer le dossier Profils.";
        return false;
    }
    const auto destination = parent / name;
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = "Un profil porte déjà ce nom.";
        return false;
    }

    const auto temporary = createTemporaryDirectory(parent);
    if (temporary.empty()) {
        error = "Impossible de préparer la copie du profil.";
        return false;
    }
    if (!copySettings(source.directory, temporary) || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        removeTemporaryDirectory(temporary);
        error = "Impossible de copier les réglages vers le nouveau profil.";
        return false;
    }
    if (!writePresetSelection(root, name, error)) {
        error = "Le profil a été créé, mais sa sélection n’a pas pu être enregistrée.";
        return false;
    }
    return true;
}

bool replacePresetDevice(const std::filesystem::path& profileRoot, const ProfilePreset& expected,
                         std::string_view oldId, std::string_view newId, DeviceReplacementError& error,
                         std::filesystem::path* recoveryDirectory, bool* feedbackDisabled)
{
    using Error = DeviceReplacementError;
    if (recoveryDirectory)
        recoveryDirectory->clear();
    if (feedbackDisabled)
        *feedbackDisabled = false;
    const auto root = extendedPath(profileRoot);
    const auto directory = extendedPath(expected.directory);
    const auto sameSelection = [&] {
        const auto selected = activePreset(root);
        return !selected.directory.empty() && selected.name == expected.name && samePathPart(selected.directory, directory);
    };
    if (directory.empty() || !plainDirectory(GetFileAttributesW(root.c_str())) || !sameSelection()
        || !isDeviceId(oldId) || !isDeviceId(newId) || oldId == newId) {
        error = Error::invalidSelection;
        return false;
    }
    const ProfileLock lock(root.parent_path());
    if (lock && hasRestoreJournal(root)) {
        error = Error::recoveryRequired;
        if (recoveryDirectory)
            *recoveryDirectory = readInterruptedProfileRestore(profileRoot).backup;
        return false;
    }
    TemporaryDirectory staging{lock ? createTemporaryDirectory(directory) : std::filesystem::path{}};
    if (staging.path.empty()) {
        error = Error::ioError;
        return false;
    }

    struct Change {
        std::filesystem::path live;
        std::filesystem::path staged;
        std::filesystem::path backup;
        SettingsSnapshot original;
        SettingsSnapshot replacement;
        bool tracked{};
        bool changed{};
        bool attempted{};
    };
    std::array<Change, settingsFiles.size()> changes{};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        auto& change = changes[i];
        change.live = directory / settingsFiles[i];
        change.staged = staging.path / settingsFiles[i];
        change.tracked = i < 2;
        if (change.tracked && (error = readSettings(change.live, change.original)) != Error::none)
            return false;
    }

    const auto isUnchanged = [](const Change& change) {
        return !change.tracked || matchesSettings(change.live, change.original);
    };
    const auto settingsUnchanged = [&] { return std::ranges::all_of(changes, isUnchanged); };
    std::string details;
    std::optional<Profile> axes;
    std::optional<AccessoryProfile> accessories;
    if (changes[0].original.present)
        axes = loadProfile(changes[0].live, details);
    if (changes[1].original.present)
        accessories = loadAccessoryProfile(changes[1].live, details);
    if (!settingsUnchanged()) {
        error = Error::invalidSelection;
        return false;
    }
    if ((!axes && changes[0].original.present) || (!accessories && changes[1].original.present)) {
        error = Error::invalidProfile;
        return false;
    }
    if (conflictingInputs(axes, accessories, oldId, newId)) {
        error = Error::conflict;
        return false;
    }
    const bool steeringChanged = axes && axes->steering.deviceId == oldId;
    if (axes) {
        for (auto* axis : {&axes->steering, &axes->throttle, &axes->brake}) {
            if (axis->deviceId == oldId)
                axis->deviceId = newId;
        }
        if (!validateProfile(*axes, details)) {
            error = Error::conflict;
            return false;
        }
    }

    for (std::size_t i = 0; i < 2; ++i) {
        auto& change = changes[i];
        auto bytes = change.original.bytes;
        change.changed = replaceDeviceTokens(bytes, i == 1, oldId, newId);
        if (!change.changed)
            continue;
        if (!writeSettings(change.staged, bytes) || readSettings(change.staged, change.replacement) != Error::none
            || !change.replacement.present) {
            error = Error::ioError;
            return false;
        }
        change.replacement.file.reset();
        if (i == 0 ? !loadProfile(change.staged, details) : !loadAccessoryProfile(change.staged, details)) {
            error = Error::invalidProfile;
            return false;
        }
    }
    if (!changes[0].changed && !changes[1].changed) {
        error = Error::deviceNotMapped;
        return false;
    }
    if (steeringChanged) {
        auto& force = changes[2];
        force.tracked = true;
        if ((error = readSettings(force.live, force.original)) != Error::none)
            return false;
        if (force.original.present) {
            DWORD readError{};
            auto settings = loadFeedbackSettings(force.live, details, &readError);
            if (!settings && readError) {
                error = Error::ioError;
                return false;
            }
            // A malformed file already disables force in the plugin. Leave it available for repair.
            force.changed = settings && settings->enabled;
            if (force.changed) {
                settings->enabled = false;
                if (!saveFeedbackSettings(force.staged, *settings, details)
                    || readSettings(force.staged, force.replacement) != Error::none || !force.replacement.present) {
                    error = Error::ioError;
                    return false;
                }
                force.replacement.file.reset();
            }
        }
    }

    const auto originals = staging.path / originalFiles;
    if (!CreateDirectoryW(originals.c_str(), nullptr)) {
        error = Error::ioError;
        return false;
    }
    for (std::size_t i = 0; i < changes.size(); ++i)
        changes[i].backup = originals / settingsFiles[i];

    if (!sameSelection() || !settingsUnchanged()) {
        error = Error::invalidSelection;
        return false;
    }
    const auto backupRoot = root / profileBackupsFolder;
    if (!ensureDirectory(backupRoot, nullptr)) {
        error = Error::ioError;
        return false;
    }
    TemporaryDirectory snapshot{createRecoveryDirectory(backupRoot, nullptr, reassignmentPrefix)};
    const auto selectedName = encodeName(expected.name);
    unsigned int files{};
    if (snapshot.path.empty() || !selectedName || !writeTextFile(snapshot.path / selectionFile, *selectedName)) {
        error = Error::ioError;
        return false;
    }
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (!changes[i].changed)
            continue;
        if (!writeSettings(snapshot.path / settingsFiles[i], changes[i].original.bytes)) {
            error = Error::ioError;
            return false;
        }
        files |= 1u << i;
    }
    if (!writeTextFile(snapshot.path / backupMarker, std::string(reassignmentVersion) + std::to_string(files))
        || !beginRecovery(root, snapshot.path)) {
        error = Error::ioError;
        return false;
    }
    auto published = std::exchange(snapshot.path, {});
    const auto cleanup = [&] {
        if (!clearRestoreJournal(root, nullptr)) {
            error = Error::recoveryRequired;
            if (recoveryDirectory)
                *recoveryDirectory = profileRoot / profileBackupsFolder / published.filename();
            return false;
        }
        snapshot.path = std::move(published);
        return true;
    };
    const auto rollback = [&](Error reason) {
        bool recovered = true;
        for (const auto i : {1u, 0u, 2u}) {
            const auto& change = changes[i];
            if (!change.attempted)
                continue;
            if (!plainFile(GetFileAttributesW(change.backup.c_str()))) {
                // If a failed replacement affected the live file, keep the durable snapshot for explicit recovery.
                recovered &= isUnchanged(change);
                continue;
            }
            const DWORD attributes = GetFileAttributesW(change.live.c_str());
            const bool missingLive = attributes == INVALID_FILE_ATTRIBUTES && missing(GetLastError());
            // A failed ReplaceFile may already have renamed the original. Do not overwrite a concurrent edit.
            if (!(missingLive || matchesSettings(change.live, change.replacement))
                || !MoveFileExW(change.backup.c_str(), change.live.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                recovered = false;
        }
        error = recovered ? reason : Error::recoveryRequired;
        if (recovered)
            cleanup();
        else {
            // ReplaceFile may have preserved an edit made by another program after our snapshot.
            staging.path.clear();
            if (recoveryDirectory)
                *recoveryDirectory = profileRoot / profileBackupsFolder / published.filename();
        }
        return false;
    };

    if (!sameSelection() || !settingsUnchanged())
        return rollback(Error::invalidSelection);
    // Stop force on the previous actuator before publishing a different steering device.
    for (const auto i : {2u, 0u, 1u}) {
        auto& change = changes[i];
        if (!change.changed)
            continue;
        if (!sameSelection() || !isUnchanged(change))
            return rollback(Error::invalidSelection);
        change.attempted = true;
        if (!ReplaceFileW(change.live.c_str(), change.staged.c_str(), change.backup.c_str(), 0, nullptr, nullptr)
            || !matchesSettings(change.backup, change.original))
            return rollback(Error::ioError);
    }
    if (!cleanup())
        return false;
    if (feedbackDisabled)
        *feedbackDisabled = changes[2].changed;
    error = Error::none;
    return true;
}

std::optional<std::string> readProfileLanguage(const std::filesystem::path& root, bool* locked)
{
    if (locked)
        *locked = false;
    if (!root.is_absolute())
        return std::nullopt;
    auto value = readSmallTextFile(root / profileLanguageFilename, locked);
    return value && validLanguage(*value) ? std::move(value) : std::nullopt;
}

bool writeProfileLanguage(const std::filesystem::path& root, std::string_view language)
{
    return writeProfileLanguageFile(root, language);
}

bool createProfileBackup(const std::filesystem::path& profileRoot, const std::filesystem::path& backupDirectory,
                         ProfileBackupError& error, std::string_view fallbackLanguage,
                         ProfileBackupFailure* detailsOutput)
{
    ProfileBackupFailure localFailure;
    auto& failure = detailsOutput ? *detailsOutput : localFailure;
    failure = {};
    const auto fail = [&](ProfileBackupStep step) {
        error = ProfileBackupError::ioError;
        failure.step = step;
        return false;
    };
    const auto root = extendedPath(profileRoot);
    const auto destination = extendedPath(backupDirectory);
    bool hasRoot{};
    if (!validBackupLocation(root, destination) || !optionalEntry(root, true, hasRoot)
        || (!fallbackLanguage.empty() && !validLanguage(fallbackLanguage))) {
        error = ProfileBackupError::invalidLocation;
        return false;
    }
    const ProfileLock lock(root.parent_path());
    if (!lock) {
        failure.windowsError = lock.failure;
        return fail(ProfileBackupStep::lock);
    }
    if (hasRestoreJournal(root)) {
        error = ProfileBackupError::recoveryRequired;
        return false;
    }
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = ProfileBackupError::destinationExists;
        return false;
    }
    const auto parent = destination.parent_path();
    const auto backups = root / profileBackupsFolder;
    if (insideOrSame(parent.lexically_normal(), backups.lexically_normal())
        && insideOrSame(backups.lexically_normal(), parent.lexically_normal())
        && (!ensureDirectory(root, &failure.windowsError) || !ensureDirectory(parent, &failure.windowsError)))
        return fail(ProfileBackupStep::prepareBackup);
    if (!plainDirectory(GetFileAttributesW(parent.c_str()))) {
        error = ProfileBackupError::invalidLocation;
        return false;
    }
    const TemporaryDirectory temporary{createTemporaryDirectory(parent, &failure.windowsError)};
    if (temporary.path.empty())
        return fail(ProfileBackupStep::prepareBackup);
    error = copySnapshot(root, temporary.path, true, &failure.windowsError);
    if (error != ProfileBackupError::none) {
        failure.step = ProfileBackupStep::copyProfile;
        return false;
    }
    if (!validSnapshot(temporary.path)) {
        error = ProfileBackupError::invalidBackup;
        return false;
    }
    std::string details;
    if ((!fallbackLanguage.empty() && !readProfileLanguage(temporary.path)
         && !writeProfileLanguageFile(temporary.path, fallbackLanguage, &failure.windowsError))
        || (!readTransmissionHotkey(temporary.path)
            && !saveTransmissionHotkey(temporary.path, defaultTransmissionHotkey, details))
        || !writeTextFile(temporary.path / backupMarker, backupVersion, &failure.windowsError))
        return fail(ProfileBackupStep::finishBackup);
    if (!MoveFileExW(temporary.path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        failure = {ProfileBackupStep::finishBackup, GetLastError()};
        error = GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES
            ? ProfileBackupError::destinationExists : ProfileBackupError::ioError;
        return false;
    }
    return true;
}

bool restoreProfileBackup(const std::filesystem::path& profileRoot, const std::filesystem::path& backupDirectory,
                          ProfileBackupError& error, std::string_view fallbackLanguage,
                          std::filesystem::path* recoveryDirectory, ProfileBackupFailure* detailsOutput)
{
    ProfileBackupFailure localFailure;
    auto& failure = detailsOutput ? *detailsOutput : localFailure;
    failure = {};
    const auto fail = [&](ProfileBackupStep step) {
        error = ProfileBackupError::ioError;
        failure.step = step;
        return false;
    };
    if (recoveryDirectory)
        recoveryDirectory->clear();
    const auto root = extendedPath(profileRoot);
    const auto source = extendedPath(backupDirectory);
    bool hasRoot{};
    if (!validBackupLocation(root, source) || !optionalEntry(root, true, hasRoot)
        || !plainDirectory(GetFileAttributesW(source.c_str()))
        || (!fallbackLanguage.empty() && !validLanguage(fallbackLanguage))) {
        error = ProfileBackupError::invalidLocation;
        return false;
    }
    const ProfileLock lock(root.parent_path());
    if (!lock) {
        failure.windowsError = lock.failure;
        return fail(ProfileBackupStep::lock);
    }

    const auto interrupted = readInterruptedProfileRestore(profileRoot);
    if (interrupted.pending) {
        if (recoveryDirectory)
            *recoveryDirectory = interrupted.backup;
        if (interrupted.backupLocked) {
            failure = {ProfileBackupStep::copyBackup, ERROR_SHARING_VIOLATION};
            error = ProfileBackupError::recoveryRequired;
            return false;
        }
        std::error_code compareError;
        if (interrupted.backupUsable
            && !std::filesystem::equivalent(source, extendedPath(interrupted.backup), compareError)) {
            error = ProfileBackupError::recoveryRequired;
            return false;
        }
    }

    const TemporaryDirectory temporary{createTemporaryDirectory(root.parent_path(), &failure.windowsError)};
    if (temporary.path.empty())
        return fail(ProfileBackupStep::prepareRestore);
    if (interrupted.deviceReassignment && interrupted.backupUsable) {
        // The targeted snapshot contains only the changed files of the affected preset.
        // Leave the journal and the intact snapshot in place if any copy or publication fails.
        const auto files = reassignmentFiles(source);
        const auto target = reassignmentTarget(root, source);
        if (!files || target.empty() || !copySettings(source, temporary.path, &failure.windowsError)
            || !validSavedSettings(temporary.path, nullptr, files)) {
            error = ProfileBackupError::recoveryRequired;
            failure.step = ProfileBackupStep::copyBackup;
            return false;
        }
        for (std::size_t i = 0; i < settingsFiles.size(); ++i) {
            const auto file = settingsFiles[i];
            if ((*files & (1u << i)) != 0 &&
                !MoveFileExW((temporary.path / file).c_str(), (target / file).c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                failure = {ProfileBackupStep::applyRestore, GetLastError()};
                error = ProfileBackupError::recoveryRequired;
                return false;
            }
        }
        if (!clearRestoreJournal(root, &failure.windowsError)) {
            error = ProfileBackupError::recoveryRequired;
            failure.step = ProfileBackupStep::applyRestore;
            return false;
        }
        error = ProfileBackupError::none;
        return true;
    }
    error = copySnapshot(source, temporary.path, false, &failure.windowsError);
    if (error != ProfileBackupError::none) {
        failure.step = ProfileBackupStep::copyBackup;
        return false;
    }
    if (!validSnapshot(temporary.path)) {
        error = ProfileBackupError::invalidBackup;
        return false;
    }
    failure.axisConflict = activeSnapshotAxisConflict(temporary.path);
    // Keep imported settings available for repair. Do not re-enable USB with a conflicting active profile.
    if (failure.axisConflict && !DeleteFileW((temporary.path / usbMarkerFilename).c_str()) && !missing(GetLastError())) {
        failure.windowsError = GetLastError();
        return fail(ProfileBackupStep::prepareRestore);
    }

    const auto backups = root / profileBackupsFolder;
    if (!ensureDirectory(root, &failure.windowsError) || !ensureDirectory(backups, &failure.windowsError))
        return fail(ProfileBackupStep::prepareRecovery);
    std::array<RestoreEntry, restoreEntries.size()> entries{};
    bool previousLanguage = false;
    bool previousHotkey = false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        const auto filename = restoreEntries[i];
        const bool directory = filename == presetsFolder;
        if (!optionalEntry(root / filename, directory, entry.present)
            || !optionalEntry(temporary.path / filename, directory, entry.incoming))
            return fail(ProfileBackupStep::prepareRestore);
        if (filename == profileLanguageFilename)
            previousLanguage = entry.present;
        else if (filename == transmissionHotkeyFilename)
            previousHotkey = entry.present;
        // Older backups can lack these installation preferences.
        if (!entry.incoming && (filename == profileLanguageFilename || filename == transmissionHotkeyFilename))
            entry.present = false;
    }

    TemporaryDirectory recovery{createRecoveryDirectory(backups, &failure.windowsError)};
    if (recovery.path.empty())
        return fail(ProfileBackupStep::prepareRecovery);
    error = copySnapshot(root, recovery.path, true, &failure.windowsError);
    if (error != ProfileBackupError::none) {
        failure.step = ProfileBackupStep::prepareRecovery;
        return false;
    }
    const auto originals = recovery.path / originalFiles;
    std::string details;
    if (!CreateDirectoryW(originals.c_str(), nullptr)) {
        failure.windowsError = GetLastError();
        return fail(ProfileBackupStep::prepareRecovery);
    }
    if (!writeTextFile(recovery.path / backupMarker, backupVersion, &failure.windowsError)
        || (!previousHotkey && !saveTransmissionHotkey(recovery.path, defaultTransmissionHotkey, details))
        || (!previousLanguage && !fallbackLanguage.empty()
            && !writeProfileLanguageFile(recovery.path, fallbackLanguage, &failure.windowsError)))
        return fail(ProfileBackupStep::prepareRecovery);

    const auto recoveryLocation = profileRoot / profileBackupsFolder / recovery.path.filename();
    const auto pendingLocation = interrupted.pending ? interrupted.backup : recoveryLocation;
    if (!interrupted.pending) {
        // Commit the recovery location before touching any live setting, including the USB marker.
        if (!beginRecovery(root, recovery.path, &failure.windowsError))
            return fail(ProfileBackupStep::prepareRecovery);
    }
    // All failures from here, including unexpected exceptions, must keep the recovery files.
    const auto publishedRecovery = std::exchange(recovery.path, {});
    const auto rollback = [&] {
        failure.step = ProfileBackupStep::applyRestore;
        bool recovered = true;
        for (std::size_t i = entries.size(); i-- > 0;) {
            if (entries[i].applied)
                recovered &= moveEntry(root, temporary.path, restoreEntries[i]);
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].saved && (recovered || restoreEntries[i] != usbMarkerFilename))
                recovered &= moveEntry(originals, root, restoreEntries[i]);
        }
        if (recovered && !interrupted.pending)
            recovered = clearRestoreJournal(root, &failure.windowsError);
        const bool stillPending = !recovered || interrupted.pending;
        error = stillPending ? ProfileBackupError::recoveryRequired : ProfileBackupError::ioError;
        if (stillPending) {
            if (recoveryDirectory)
                *recoveryDirectory = pendingLocation;
        } else {
            removeTemporaryDirectory(publishedRecovery);
        }
        return false;
    };
    for (std::size_t i = entries.size(); i-- > 0;) {
        if (!entries[i].present)
            continue;
        if (!moveEntry(root, originals, restoreEntries[i], &failure.windowsError))
            return rollback();
        entries[i].saved = true;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].incoming)
            continue;
        if (!moveEntry(temporary.path, root, restoreEntries[i], &failure.windowsError))
            return rollback();
        entries[i].applied = true;
    }
    if (!clearRestoreJournal(root, &failure.windowsError)) {
        failure.step = ProfileBackupStep::applyRestore;
        error = ProfileBackupError::recoveryRequired;
        if (recoveryDirectory)
            *recoveryDirectory = pendingLocation;
        return false;
    }
    if (recoveryDirectory)
        *recoveryDirectory = recoveryLocation;
    return true;
}

} // namespace chrome_axis
