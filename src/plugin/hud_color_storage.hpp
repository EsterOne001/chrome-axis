#pragma once

#include "../core/settings_replace.hpp"

#include <memory>
#include <string_view>

namespace chrome_axis {

enum class HudColorCommitResult : std::int32_t { success = 0, retry = 1, conflict = 2, empty = 3,
                                                tooLarge = 4, unavailable = 5, recoveryRequired = 6 };

namespace detail {

inline constexpr DWORD maxHudColorBytes = 4 * 1024 * 1024;

struct HudColorHandleCloser {
    void operator()(HANDLE file) const noexcept { CloseHandle(file); }
};

using HudColorHandle = std::unique_ptr<void, HudColorHandleCloser>;

inline HudColorHandle hudColorHandle(HANDLE file) noexcept {
    return HudColorHandle{file == INVALID_HANDLE_VALUE ? nullptr : file};
}

struct HudColorDocument {
    HudColorCommitResult result;
    std::string bytes;
};

inline HudColorCommitResult hudColorFileError(DWORD error) noexcept {
    switch (error) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return HudColorCommitResult::retry;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return HudColorCommitResult::conflict;
    default:
        return HudColorCommitResult::unavailable;
    }
}

inline HudColorDocument readHudColorBytes(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0)
        return {HudColorCommitResult::unavailable, {}};
    if (size.QuadPart == 0)
        return {HudColorCommitResult::empty, {}};
    if (size.QuadPart > maxHudColorBytes)
        return {HudColorCommitResult::tooLarge, {}};
    if (!SetFilePointerEx(file, LARGE_INTEGER{}, nullptr, FILE_BEGIN))
        return {hudColorFileError(GetLastError()), {}};
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD count{};
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr))
        return {hudColorFileError(GetLastError()), {}};
    if (count != text.size())
        return {HudColorCommitResult::retry, {}};
    return {HudColorCommitResult::success, std::move(text)};
}

inline HudColorDocument readHudColorFile(const std::filesystem::path& path,
                                         DWORD sharing = FILE_SHARE_READ | FILE_SHARE_DELETE) {
    const auto file = hudColorHandle(CreateFileW(path.c_str(), GENERIC_READ, sharing,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return {hudColorFileError(GetLastError()), {}};
    return readHudColorBytes(file.get());
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

inline std::filesystem::path hudColorFile(const std::filesystem::path& gameRoot) {
    return gameRoot / L"r6" / L"storages" / L"ChromeAxisHUD" / L"colors.json";
}

inline std::filesystem::path hudColorSidecar(const std::filesystem::path& target, const wchar_t* suffix) {
    auto path = target;
    path += suffix;
    return path;
}

inline HudColorHandle lockHudColorFile(const std::filesystem::path& target, HudColorCommitResult& result) {
    const auto path = hudColorSidecar(target, L".lock");
    auto handle = hudColorHandle(CreateFileW(path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_ALWAYS,
                                            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    result = handle ? HudColorCommitResult::success : hudColorFileError(GetLastError());
    return handle;
}

inline bool missingHudColorFile(const std::filesystem::path& path) noexcept {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Never restore a backup over a target that another writer has put back.
inline bool restoreMissingHudColorFile(const std::filesystem::path& target,
                                        const std::filesystem::path& backup) noexcept {
    return missingHudColorFile(target) && MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
}

inline HudColorCommitResult recoverHudColorFile(const std::filesystem::path& target,
                                                const std::filesystem::path& backup) {
    if (missingHudColorFile(backup))
        return HudColorCommitResult::success;
    if (missingHudColorFile(target)) {
        return MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)
            ? HudColorCommitResult::conflict : hudColorFileError(GetLastError());
    }
    return HudColorCommitResult::recoveryRequired;
}

inline HudColorCommitResult requireHudColorRecovery(const std::filesystem::path& target,
                                                     const std::filesystem::path& backup) {
    const auto pending = hudColorSidecar(target, L".recovery");
    // If rollback already moved the original backup, retain its replacement under the fixed name.
    // A failed move leaves the uniquely named recovery file intact.
    if (backup != pending && missingHudColorFile(pending))
        MoveFileExW(backup.c_str(), pending.c_str(), MOVEFILE_WRITE_THROUGH);
    return HudColorCommitResult::recoveryRequired;
}

inline HudColorCommitResult restoreDisplacedHudColors(const std::filesystem::path& target,
                                                       std::filesystem::path backup, std::string_view published) {
    auto candidate = std::string(published);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto current = readHudColorFile(target);
        if (current.result != HudColorCommitResult::success || current.bytes != candidate)
            return restoreMissingHudColorFile(target, backup)
                ? HudColorCommitResult::conflict : requireHudColorRecovery(target, backup);
        auto toRestore = readHudColorFile(backup);
        const auto displaced = stagedSettingsPath(hudColorSidecar(target, L".recovery"));
        if (!ReplaceFileW(target.c_str(), backup.c_str(), displaced.c_str(), 0, nullptr, nullptr)) {
            // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 can leave the live path empty even during rollback.
            // Restore the last writer in that case, unless we know only our candidate was moved.
            const auto moved = readHudColorFile(displaced);
            const bool knownCandidate = moved.result == HudColorCommitResult::success && moved.bytes == candidate;
            if (!restoreMissingHudColorFile(target, knownCandidate ? backup : displaced))
                return requireHudColorRecovery(target, moved.result == HudColorCommitResult::conflict ? backup : displaced);
            if (knownCandidate && attempt == 0)
                DeleteFileW(displaced.c_str());
            return HudColorCommitResult::conflict;
        }
        const auto replaced = readHudColorFile(displaced);
        if (replaced.result == HudColorCommitResult::success && replaced.bytes == candidate) {
            if (attempt == 0) // Only our original candidate is safe to discard automatically.
                DeleteFileW(displaced.c_str());
            return HudColorCommitResult::conflict;
        }
        if (toRestore.result != HudColorCommitResult::success)
            return requireHudColorRecovery(target, displaced);
        // A second writer arrived during restoration. Retry from the version it actually displaced.
        candidate = std::move(toRestore.bytes);
        backup = displaced;
    }
    return requireHudColorRecovery(target, backup);
}

} // namespace detail

// Called after an absent or unreadable result from RedFileSystem, not during normal reading.
inline HudColorCommitResult recoverHudColors(const std::filesystem::path& gameRoot) {
    using Result = HudColorCommitResult;
    if (gameRoot.empty())
        return Result::unavailable;
    const auto target = detail::hudColorFile(gameRoot);
    const auto backup = detail::hudColorSidecar(target, L".recovery");
    if (detail::missingHudColorFile(backup)) {
        if (detail::missingHudColorFile(target))
            return Result::success;
        return detail::readHudColorFile(target, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE).result;
    }
    Result lockResult{};
    const auto lock = detail::lockHudColorFile(target, lockResult);
    if (!lock)
        return lockResult;
    return detail::recoverHudColorFile(target, backup);
}

// The caller supplies JSON serialized by RedData and the exact text it merged.
// Keep the old file available until the new one has been written and verified.
inline HudColorCommitResult commitHudColors(const std::filesystem::path& gameRoot, std::string_view json,
                                             std::string_view previousJson, bool existed) {
    using Result = HudColorCommitResult;
    if (gameRoot.empty())
        return Result::unavailable;
    if (json.size() > detail::maxHudColorBytes || previousJson.size() > detail::maxHudColorBytes)
        return Result::tooLarge;
    if (json.empty() || (existed && previousJson.empty()))
        return Result::empty;

    const auto target = detail::hudColorFile(gameRoot);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error)
        return Result::unavailable;

    Result lockResult{};
    const auto lock = detail::lockHudColorFile(target, lockResult);
    if (!lock)
        return lockResult;
    const auto backup = detail::hudColorSidecar(target, L".recovery");
    if (const auto recovered = detail::recoverHudColorFile(target, backup); recovered != Result::success)
        return recovered;

    if (existed) {
        const auto observed = detail::readHudColorFile(target);
        if (observed.result != Result::success)
            return observed.result;
        if (!detail::sameHudColorText(observed.bytes, previousJson))
            return Result::conflict;
    }

    const auto staged = detail::stagedSettingsPath(target);
    auto output = detail::hudColorHandle(CreateFileW(staged.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                                     nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output)
        return detail::hudColorFileError(GetLastError());
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
        || !FlushFileBuffers(output.get()))
        return detail::hudColorFileError(GetLastError());
    if (count != json.size())
        return Result::unavailable;
    const auto saved = detail::readHudColorBytes(output.get());
    if (saved.result != Result::success || saved.bytes != json)
        return Result::unavailable;
    output.reset();

    if (!existed)
        return MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)
            ? Result::success : detail::hudColorFileError(GetLastError());

    // Win32 does not offer a conditional replace. Inspect the file actually displaced by ReplaceFileW.
    if (!ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
        const auto failure = GetLastError();
        if (detail::restoreMissingHudColorFile(target, backup))
            return Result::conflict;
        if (!detail::missingHudColorFile(backup))
            return Result::recoveryRequired;
        return detail::hudColorFileError(failure);
    }
    const auto displaced = detail::readHudColorFile(backup);
    if (displaced.result == Result::success && detail::sameHudColorText(displaced.bytes, previousJson)) {
        DeleteFileW(backup.c_str());
        return Result::success;
    }
    return detail::restoreDisplacedHudColors(target, backup, json);
}

} // namespace chrome_axis
