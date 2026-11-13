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

namespace chrome_axis {
namespace {

constexpr wchar_t presetsFolder[] = L"Profils";
constexpr wchar_t selectionFile[] = L"active_profile.txt";
constexpr wchar_t backupMarker[] = L"chrome_axis_backup.txt";
constexpr wchar_t backupLock[] = L".chrome_axis_profile_backup.lock";
constexpr wchar_t originalFiles[] = L"Fichiers-originaux";
constexpr std::string_view backupVersion = "Chrome Axis profile backup 1";
constexpr std::size_t maxNameLength = 64;
constexpr std::array settingsFiles{profileFilename, accessoryFilename, feedbackFilename};
constexpr std::array globalFiles{selectionFile, profileLanguageFilename, usbMarkerFilename};
// Apply the USB marker last. Move it away first when taking out the previous settings.
constexpr std::array restoreEntries{profileFilename, accessoryFilename, feedbackFilename, presetsFolder,
                                    selectionFile, profileLanguageFilename, usbMarkerFilename};
std::atomic<unsigned long> temporaryId{};

bool missing(DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
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

std::optional<std::string> readSmallTextFile(const std::filesystem::path& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return missing(GetLastError()) ? std::optional(std::string{}) : std::nullopt;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
    BY_HANDLE_FILE_INFORMATION information{};
    std::array<char, maxNameLength * 4 + 8> bytes{};
    DWORD count{};
    if (!GetFileInformationByHandle(handle, &information) || !plainFile(information.dwFileAttributes)
        || !ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count >= bytes.size())
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

std::optional<std::wstring> readSelection(const std::filesystem::path& path)
{
    const auto contents = readSmallTextFile(path);
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

bool writeTextFile(const std::filesystem::path& target, std::string_view contents)
{
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (!plainFile(attributes) && (attributes != INVALID_FILE_ATTRIBUTES || !missing(GetLastError())))
        return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto temporary = temporaryPath(target.parent_path());
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return false;
        }

        auto text = std::string(contents);
        text += '\n';
        const auto length = static_cast<DWORD>(text.size());
        DWORD written{};
        const bool complete = WriteFile(file, text.data(), length, &written, nullptr) && written == length
            && FlushFileBuffers(file);
        CloseHandle(file);
        if (complete && MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        DeleteFileW(temporary.c_str());
        return false;
    }
    return false;
}

std::filesystem::path createTemporaryDirectory(const std::filesystem::path& parent)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto path = temporaryPath(parent);
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    return {};
}

bool copySavedFile(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && missing(GetLastError()))
        return true;
    if (!plainFile(attributes) || !CopyFileExW(source.c_str(), destination.c_str(),
                                              nullptr, nullptr, nullptr, COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK))
        return false;
    const DWORD copiedAttributes = GetFileAttributesW(destination.c_str());
    if (!plainFile(copiedAttributes))
        return false;
    // An exported or named copy needs to remain editable even if the source is read-only.
    const auto writable = copiedAttributes & ~FILE_ATTRIBUTE_READONLY;
    return copiedAttributes == writable ||
        SetFileAttributesW(destination.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL);
}

bool copySettings(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    return std::ranges::all_of(settingsFiles, [&](const auto filename) {
        return copySavedFile(source / filename, destination / filename);
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

bool ensureDirectory(const std::filesystem::path& path)
{
    return CreateDirectoryW(path.c_str(), nullptr) || plainDirectory(GetFileAttributesW(path.c_str()));
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

struct BackupLock {
    explicit BackupLock(const std::filesystem::path& parent)
        : handle(CreateFileW((parent / backupLock).c_str(), DELETE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr))
    {}

    HANDLE handle;

    ~BackupLock()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    explicit operator bool() const noexcept { return handle != INVALID_HANDLE_VALUE; }
};

ProfileBackupError copySnapshot(const std::filesystem::path& source, const std::filesystem::path& destination,
                                bool allowEmpty)
{
    bool foundProfile = false;
    const auto copyFile = [&](const wchar_t* filename, bool profileSetting) {
        bool exists{};
        if (!optionalEntry(source / filename, false, exists))
            return ProfileBackupError::invalidBackup;
        if (exists && !copySavedFile(source / filename, destination / filename))
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
        if (!ensureDirectory(target))
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
            if (!CreateDirectoryW((target / name).c_str(), nullptr) || !copySettings(it->path(), target / name))
                return ProfileBackupError::ioError;
            foundProfile = true;
        }
        if (error)
            return ProfileBackupError::ioError;
    }

    bool marked{};
    if (!optionalEntry(source / backupMarker, false, marked)
        || (marked && readSmallTextFile(source / backupMarker) != backupVersion))
        return ProfileBackupError::invalidBackup;
    auto path = source.lexically_normal();
    if (!path.has_filename())
        path = path.parent_path();
    const bool legacy = samePathPart(path.filename(), L"Profil");
    return foundProfile || marked || allowEmpty || legacy ? ProfileBackupError::none : ProfileBackupError::invalidBackup;
}

bool validSavedSettings(const std::filesystem::path& path)
{
    std::string error;
    bool hasAxes{}, hasAccessories{}, hasFeedback{};
    return optionalEntry(path / profileFilename, false, hasAxes)
        && (!hasAxes || loadProfile(path / profileFilename, error))
        && optionalEntry(path / accessoryFilename, false, hasAccessories)
        && (!hasAccessories || loadAccessoryProfile(path / accessoryFilename, error))
        && optionalEntry(path / feedbackFilename, false, hasFeedback)
        && (!hasFeedback || loadFeedbackSettings(path / feedbackFilename, error));
}

bool validSnapshot(const std::filesystem::path& path)
{
    const auto selected = readSelection(path / selectionFile);
    bool hasLanguage{};
    if (!selected || presetDirectory(path, *selected).empty() || !validSavedSettings(path)
        || !optionalEntry(path / profileLanguageFilename, false, hasLanguage) || (hasLanguage && !readProfileLanguage(path)))
        return false;
    return std::ranges::all_of(listPresets(path), [&](const auto& name) {
        return validSavedSettings(path / presetsFolder / name);
    });
}

std::filesystem::path createRecoveryDirectory(const std::filesystem::path& parent)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto name = std::format(L"Avant-restauration-{:04}{:02}{:02}-{:02}{:02}{:02}", time.wYear, time.wMonth,
                                  time.wDay, time.wHour, time.wMinute, time.wSecond);
    for (int index = 0; index < 100; ++index) {
        const auto path = parent / (index == 0 ? name : name + L"-" + std::to_wstring(index + 1));
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    return {};
}

struct RestoreEntry {
    bool present{};
    bool incoming{};
    bool saved{};
    bool applied{};
};

bool moveEntry(const std::filesystem::path& from, const std::filesystem::path& to, const wchar_t* filename)
{
    return MoveFileExW((from / filename).c_str(), (to / filename).c_str(), MOVEFILE_WRITE_THROUGH);
}

} // namespace

ProfilePreset activePreset(const std::filesystem::path& root)
{
    if (!root.is_absolute())
        return {};
    const auto selected = readSelection(root / selectionFile);
    if (!selected)
        return {};
    return {*selected, presetDirectory(root, *selected)};
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

bool selectPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error)
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

bool createPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error)
{
    error.clear();
    if (!validName(name) || !encodeName(name)) {
        error = "Saisissez un nom de profil valide (64 caractères maximum).";
        return false;
    }
    const auto source = activePreset(root);
    if (source.directory.empty()) {
        error = "Sélectionnez un profil existant avant d’en créer une copie.";
        return false;
    }

    const auto parent = root / presetsFolder;
    std::error_code ioError;
    std::filesystem::create_directories(root, ioError);
    if (ioError || (!CreateDirectoryW(parent.c_str(), nullptr) && !plainDirectory(GetFileAttributesW(parent.c_str())))) {
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
    if (!selectPreset(root, name, error)) {
        error = "Le profil a été créé, mais sa sélection n’a pas pu être enregistrée.";
        return false;
    }
    return true;
}

std::optional<std::string> readProfileLanguage(const std::filesystem::path& root)
{
    if (!root.is_absolute())
        return std::nullopt;
    auto value = readSmallTextFile(root / profileLanguageFilename);
    return value && validLanguage(*value) ? std::move(value) : std::nullopt;
}

bool writeProfileLanguage(const std::filesystem::path& root, std::string_view language)
{
    return root.is_absolute() && validLanguage(language) && plainDirectory(GetFileAttributesW(root.c_str()))
        && writeTextFile(root / profileLanguageFilename, language);
}

bool createProfileBackup(const std::filesystem::path& profileRoot, const std::filesystem::path& backupDirectory,
                         ProfileBackupError& error, std::string_view fallbackLanguage)
{
    const auto root = extendedPath(profileRoot);
    const auto destination = extendedPath(backupDirectory);
    bool hasRoot{};
    if (!validBackupLocation(root, destination) || !optionalEntry(root, true, hasRoot)
        || (!fallbackLanguage.empty() && !validLanguage(fallbackLanguage))) {
        error = ProfileBackupError::invalidLocation;
        return false;
    }
    const BackupLock lock(root.parent_path());
    if (!lock) {
        error = ProfileBackupError::ioError;
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
        && (!ensureDirectory(root) || !ensureDirectory(parent))) {
        error = ProfileBackupError::ioError;
        return false;
    }
    if (!plainDirectory(GetFileAttributesW(parent.c_str()))) {
        error = ProfileBackupError::invalidLocation;
        return false;
    }
    const TemporaryDirectory temporary{createTemporaryDirectory(parent)};
    if (temporary.path.empty()) {
        error = ProfileBackupError::ioError;
        return false;
    }
    error = copySnapshot(root, temporary.path, true);
    if (error != ProfileBackupError::none)
        return false;
    if (!validSnapshot(temporary.path)) {
        error = ProfileBackupError::invalidBackup;
        return false;
    }
    if ((!fallbackLanguage.empty() && !readProfileLanguage(temporary.path)
         && !writeProfileLanguage(temporary.path, fallbackLanguage))
        || !writeTextFile(temporary.path / backupMarker, backupVersion)) {
        error = ProfileBackupError::ioError;
        return false;
    }
    if (!MoveFileExW(temporary.path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES
            ? ProfileBackupError::destinationExists : ProfileBackupError::ioError;
        return false;
    }
    return true;
}

bool restoreProfileBackup(const std::filesystem::path& profileRoot, const std::filesystem::path& backupDirectory,
                          ProfileBackupError& error, std::string_view fallbackLanguage,
                          std::filesystem::path* recoveryDirectory)
{
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
    const BackupLock lock(root.parent_path());
    if (!lock) {
        error = ProfileBackupError::ioError;
        return false;
    }

    const TemporaryDirectory temporary{createTemporaryDirectory(root.parent_path())};
    if (temporary.path.empty()) {
        error = ProfileBackupError::ioError;
        return false;
    }
    error = copySnapshot(source, temporary.path, false);
    if (error != ProfileBackupError::none)
        return false;
    if (!validSnapshot(temporary.path)) {
        error = ProfileBackupError::invalidBackup;
        return false;
    }

    const auto backups = root / profileBackupsFolder;
    if (!ensureDirectory(root) || !ensureDirectory(backups)) {
        error = ProfileBackupError::ioError;
        return false;
    }
    std::array<RestoreEntry, restoreEntries.size()> entries{};
    bool previousLanguage = false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        const auto filename = restoreEntries[i];
        const bool directory = filename == presetsFolder;
        if (!optionalEntry(root / filename, directory, entry.present)
            || !optionalEntry(temporary.path / filename, directory, entry.incoming)) {
            error = ProfileBackupError::ioError;
            return false;
        }
        if (filename == profileLanguageFilename) {
            previousLanguage = entry.present;
            // A manually saved Profil folder from an older version lacks the language choice.
            if (!entry.incoming)
                entry.present = false;
        }
    }

    TemporaryDirectory recovery{createRecoveryDirectory(backups)};
    if (recovery.path.empty()) {
        error = ProfileBackupError::ioError;
        return false;
    }
    error = copySnapshot(root, recovery.path, true);
    if (error != ProfileBackupError::none)
        return false;
    const auto originals = recovery.path / originalFiles;
    if (!CreateDirectoryW(originals.c_str(), nullptr)
        || !writeTextFile(recovery.path / backupMarker, backupVersion)
        || (!previousLanguage && !fallbackLanguage.empty() && !writeProfileLanguage(recovery.path, fallbackLanguage))) {
        error = ProfileBackupError::ioError;
        return false;
    }

    const auto recoveryLocation = profileRoot / profileBackupsFolder / recovery.path.filename();
    const auto rollback = [&] {
        bool recovered = true;
        for (std::size_t i = entries.size(); i-- > 0;) {
            if (entries[i].applied)
                recovered &= moveEntry(root, temporary.path, restoreEntries[i]);
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].saved && (recovered || restoreEntries[i] != usbMarkerFilename))
                recovered &= moveEntry(originals, root, restoreEntries[i]);
        }
        error = recovered ? ProfileBackupError::ioError : ProfileBackupError::recoveryRequired;
        if (!recovered) {
            if (recoveryDirectory)
                *recoveryDirectory = recoveryLocation;
            recovery.path.clear();
        }
        return false;
    };
    for (std::size_t i = entries.size(); i-- > 0;) {
        if (!entries[i].present)
            continue;
        if (!moveEntry(root, originals, restoreEntries[i]))
            return rollback();
        entries[i].saved = true;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].incoming)
            continue;
        if (!moveEntry(temporary.path, root, restoreEntries[i]))
            return rollback();
        entries[i].applied = true;
    }
    if (recoveryDirectory)
        *recoveryDirectory = recoveryLocation;
    recovery.path.clear();
    return true;
}

} // namespace chrome_axis
