#include "chrome_axis/presets.hpp"

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <system_error>

namespace chrome_axis {
namespace {

constexpr wchar_t presetsFolder[] = L"Profils";
constexpr wchar_t selectionFile[] = L"active_profile.txt";
constexpr std::size_t maxNameLength = 64;
constexpr std::array settingsFiles{profileFilename, accessoryFilename, feedbackFilename};
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

std::optional<std::wstring> readSelection(const std::filesystem::path& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return missing(GetLastError()) ? std::optional(std::wstring{}) : std::nullopt;
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
    if (text.empty())
        return std::wstring{};
    const auto length = static_cast<int>(text.size());
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (units <= 0 || units > static_cast<int>(maxNameLength))
        return std::nullopt;
    std::wstring name(static_cast<std::size_t>(units), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, name.data(), units) || !validName(name))
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

bool writeSelection(const std::filesystem::path& root, const std::string& name)
{
    const auto target = root / selectionFile;
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (!plainFile(attributes) && (attributes != INVALID_FILE_ATTRIBUTES || !missing(GetLastError())))
        return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto temporary = temporaryPath(root);
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return false;
        }

        const auto text = name + '\n';
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

bool copySettings(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    for (const auto filename : settingsFiles) {
        const auto file = source / filename;
        const auto copy = destination / filename;
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && missing(GetLastError()))
            continue;
        if (!plainFile(attributes) || !CopyFileW(file.c_str(), copy.c_str(), TRUE))
            return false;
        const DWORD copiedAttributes = GetFileAttributesW(copy.c_str());
        // Windows copies the read-only bit. A named copy needs to remain editable.
        if (!plainFile(copiedAttributes))
            return false;
        if ((copiedAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            const auto writable = copiedAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (!SetFileAttributesW(copy.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL))
                return false;
        }
    }
    return true;
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
    if (!encoded || ioError || !writeSelection(root, *encoded)) {
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

} // namespace chrome_axis
