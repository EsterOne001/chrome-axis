#include <windows.h>

// Interpose at the final OS calls in this executable only. After injecting a competing
// rename we call the real ReplaceFileW, preserving Windows sharing and backup semantics.
BOOL WINAPI testReplaceHudColors(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
BOOL WINAPI testWriteHudColors(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
HANDLE WINAPI testFindHudRecovery(LPCWSTR, LPWIN32_FIND_DATAW);
#define ReplaceFileW testReplaceHudColors
#define WriteFile testWriteHudColors
#define FindFirstFileW testFindHudRecovery
#include "hud_color_storage.hpp"
#undef FindFirstFileW
#undef WriteFile
#undef ReplaceFileW

#include <fstream>
#include <iostream>
#include <iterator>

namespace {

constexpr std::string_view original = R"({"presets":{"night":{"transmission":[255,0,0]}}})";
constexpr std::string_view edited = R"({"presets":{"night":{"transmission":[255,0,0],"border":[0,255,126]}}})";
constexpr std::string_view external = R"({"presets":{"Préréglage":{"border":[15,10,20]}}})";
constexpr std::string_view merged = R"({"presets":{"Préréglage":{"border":[15,10,20]},"night":{"border":[0,255,126]}}})";
constexpr std::string_view later = R"({"presets":{"Dernier":{"transmission":[20,30,40]}}})";

enum class Interruption { none, concurrentReplace, concurrentRestore, concurrentReplaceLater,
                          replacementDidNotMove, rollbackDidNotMove, lockedDuringRollback,
                          interruptAfterRollback, interruptDuringRollback, interruptAfterSecondWriter,
                          interruptDuringSecondAttempt, interruptAfterDisplacement };
struct InterruptedSave {};
Interruption interruption{};
bool injected{};
bool shortWrite{};
bool listDenied{};
chrome_axis::detail::HudColorHandle rollbackLock;

bool publishExternal(const std::filesystem::path& path, std::string_view contents) {
    auto staged = path;
    staged += L".external-test";
    {
        std::ofstream output(staged, std::ios::binary);
        output << contents;
        if (!output)
            return false;
    }
    return MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool check(bool condition, const char* description) {
    if (!condition)
        std::cerr << description << '\n';
    return condition;
}

} // namespace

BOOL WINAPI testReplaceHudColors(LPCWSTR target, LPCWSTR staged, LPCWSTR backup, DWORD flags,
                                 LPVOID exclude, LPVOID reserved) {
    if (interruption == Interruption::interruptAfterDisplacement) {
        interruption = Interruption::none;
        injected = injected && MoveFileExW(target, backup, MOVEFILE_WRITE_THROUGH) != 0;
        throw InterruptedSave{};
    }
    if (interruption == Interruption::interruptAfterSecondWriter || interruption == Interruption::interruptDuringSecondAttempt) {
        const bool stopAfter = interruption == Interruption::interruptAfterSecondWriter;
        injected = injected && publishExternal(target, later);
        injected = ReplaceFileW(target, staged, backup, flags, exclude, reserved) != 0 && injected;
        interruption = stopAfter ? Interruption::none : Interruption::interruptAfterDisplacement;
        if (stopAfter)
            throw InterruptedSave{};
        return injected ? TRUE : FALSE;
    }
    if (interruption == Interruption::interruptAfterRollback || interruption == Interruption::interruptDuringRollback) {
        interruption = interruption == Interruption::interruptAfterRollback ? Interruption::interruptAfterSecondWriter
                                                                              : Interruption::interruptDuringSecondAttempt;
        injected = publishExternal(target, external);
        const auto replaced = ReplaceFileW(target, staged, backup, flags, exclude, reserved);
        injected = injected && replaced != 0;
        return replaced;
    }
    if (interruption == Interruption::replacementDidNotMove) {
        interruption = Interruption::none;
        injected = MoveFileExW(target, backup, MOVEFILE_WRITE_THROUGH) != 0;
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return FALSE;
    }
    if (interruption == Interruption::concurrentReplace) {
        interruption = Interruption::none;
        injected = publishExternal(target, external);
    } else if (interruption == Interruption::concurrentReplaceLater) {
        interruption = Interruption::none;
        injected = injected && publishExternal(target, later);
    } else if (interruption == Interruption::lockedDuringRollback) {
        interruption = Interruption::none;
        injected = publishExternal(target, external);
        const auto replaced = ReplaceFileW(target, staged, backup, flags, exclude, reserved);
        rollbackLock = chrome_axis::detail::hudColorHandle(CreateFileW(target, GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        injected = injected && static_cast<bool>(rollbackLock);
        return replaced;
    } else if (interruption == Interruption::concurrentRestore || interruption == Interruption::rollbackDidNotMove) {
        const auto duringRollback = interruption == Interruption::concurrentRestore
            ? Interruption::concurrentReplaceLater : Interruption::replacementDidNotMove;
        interruption = Interruption::none;
        injected = publishExternal(target, external);
        const auto replaced = ReplaceFileW(target, staged, backup, flags, exclude, reserved);
        interruption = duringRollback;
        return replaced;
    }
    return ReplaceFileW(target, staged, backup, flags, exclude, reserved);
}

BOOL WINAPI testWriteHudColors(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD actual, LPOVERLAPPED overlapped) {
    if (shortWrite) {
        shortWrite = false;
        size /= 2;
    }
    return WriteFile(file, bytes, size, actual, overlapped);
}

HANDLE WINAPI testFindHudRecovery(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    if (listDenied) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileW(path, data);
}

int main() {
    using chrome_axis::commitHudColors;
    using chrome_axis::recoverHudColors;
    using Result = chrome_axis::HudColorCommitResult;
    const auto game = chrome_axis::detail::stagedSettingsPath(std::filesystem::temp_directory_path()
                                                              / L"chrome-axis-hud-test");
    if (!std::filesystem::create_directory(game))
        return 1;
    struct Fixture {
        const std::filesystem::path& root;
        ~Fixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } fixture{game};
    const auto directory = game / L"r6" / L"storages" / L"ChromeAxisHUD";
    const auto colors = directory / L"colors.json";
    const auto backup = directory / L"colors.json.recovery";

    bool ok = check(recoverHudColors(game) == Result::success && !std::filesystem::exists(directory),
                    "A fresh installation must not create lock files during recovery.");
    ok &= check(commitHudColors(game, original, {}, false) == Result::success && read(colors) == original,
                    "A first save must create the documented color file.");
    ok &= check(commitHudColors(game, edited, {}, false) == Result::conflict && read(colors) == original,
                "A file appearing since the initial read must not be overwritten.");

    const auto restrictedGame = game / L"list-permission";
    const auto restrictedColors = chrome_axis::detail::hudColorFile(restrictedGame);
    const auto restrictedBackup = chrome_axis::detail::hudColorSidecar(restrictedColors, L".recovery");
    ok &= check(commitHudColors(restrictedGame, original, {}, false) == Result::success,
                "Could not prepare the existing readable color document.");
    std::ofstream(restrictedBackup) << external;
    listDenied = true;
    ok &= check(recoverHudColors(restrictedGame, false) == Result::recoveryScanDenied &&
                    recoverHudColors(restrictedGame) == Result::recoveryScanDenied && read(restrictedColors) == original &&
                    commitHudColors(restrictedGame, edited, original, true) == Result::recoveryScanDenied &&
                    read(restrictedBackup) == external && read(restrictedColors) == original,
                "Denied enumeration must allow a direct read without merging or replacing either document.");
    {
        const auto lock = chrome_axis::detail::hudColorHandle(CreateFileW(restrictedColors.c_str(), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ok &= check(static_cast<bool>(lock) && recoverHudColors(restrictedGame) == Result::retry,
                    "The fallback must distinguish a sharing violation from denied enumeration.");
    }
    listDenied = false;
    ok &= check(recoverHudColors(restrictedGame) == Result::recoveryRequired &&
                    commitHudColors(restrictedGame, edited, original, true) == Result::recoveryRequired &&
                    read(restrictedBackup) == external,
                "Restoring list permission must still expose the pending recovery file before any write.");
    ok &= check(DeleteFileW(restrictedBackup.c_str()) && publishExternal(restrictedColors, external) &&
                    recoverHudColors(restrictedGame, false) == Result::success &&
                    commitHudColors(restrictedGame, edited, original, true) == Result::conflict &&
                    commitHudColors(restrictedGame, merged, external, true) == Result::success && read(restrictedColors) == merged,
                "After permission returns, retry must compare the actual document and preserve the external change.");

    {
        const auto lock = chrome_axis::detail::hudColorHandle(CreateFileW(colors.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ok &= check(static_cast<bool>(lock), "Could not simulate a reader that prevents replacement.");
        ok &= check(commitHudColors(game, edited, original, true) == Result::retry && read(colors) == original,
                    "A locked file must retain its original data.");
    }
    {
        const auto lock = chrome_axis::detail::hudColorHandle(CreateFileW(colors.c_str(), GENERIC_READ,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ok &= check(static_cast<bool>(lock) && recoverHudColors(game) == Result::retry
                        && recoverHudColors(game, false) == Result::success,
                    "A full probe must detect a lock while a checkpoint-only probe avoids reopening the extant file.");
    }
    {
        Result lockResult{};
        const auto lock = chrome_axis::detail::lockHudColorFile(colors, lockResult);
        ok &= check(lockResult == Result::success && commitHudColors(game, edited, original, true) == Result::retry,
                    "A parallel Chrome Axis writer must report transient lock contention.");
    }
    ok &= check(commitHudColors(game, edited, original, true) == Result::success && read(colors) == edited,
                "The pending colors must replace the original file after the lock is released.");

    ok &= check(commitHudColors(game, external, edited, true) == Result::success && read(colors) == external,
                "An unrelated preset with a UTF-8 name must remain byte-for-byte intact when written.");
    ok &= check(commitHudColors(game, edited, original, true) == Result::conflict && read(colors) == external,
                "A stale merge must leave the more recent data untouched.");

    constexpr auto prettyWindows = "{\r\n  \"presets\": {}\r\n}\r\n";
    constexpr auto prettyRead = "{\n  \"presets\": {}\n}\n";
    ok &= check(commitHudColors(game, prettyWindows, external, true) == Result::success
                    && commitHudColors(game, edited, prettyRead, true) == Result::success && read(colors) == edited,
                "The compare step must accept Windows line endings translated by RedFileSystem.");

    interruption = Interruption::concurrentReplace;
    injected = false;
    ok &= check(commitHudColors(game, original, edited, true) == Result::conflict && injected
                    && read(colors) == external,
                "A real update immediately before publication must be restored, and the merge retried.");
    ok &= check(commitHudColors(game, merged, external, true) == Result::success && read(colors) == merged,
                "Retrying against the newly published version must preserve both edits.");

    shortWrite = true;
    ok &= check(commitHudColors(game, edited, merged, true) == Result::unavailable && !shortWrite
                    && read(colors) == merged,
                "An incomplete candidate must never replace the previous document.");

    interruption = Interruption::replacementDidNotMove;
    injected = false;
    ok &= check(commitHudColors(game, edited, merged, true) == Result::conflict && injected
                    && read(colors) == merged && !std::filesystem::exists(backup),
                "A partial Windows replacement must recover the displaced document without overwriting a new one.");

    ok &= check(MoveFileExW(colors.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)
                    && recoverHudColors(game) == Result::conflict && read(colors) == merged,
                "The sole surviving recovery file from a stopped process must be restored on the next read.");

    const auto wrongTypeGame = game / L"directory-checkpoint";
    const auto wrongTypeColors = chrome_axis::detail::hudColorFile(wrongTypeGame);
    const auto wrongTypeBackup = chrome_axis::detail::hudColorSidecar(wrongTypeColors, L".recovery");
    std::filesystem::create_directories(wrongTypeBackup);
    std::ofstream(wrongTypeBackup / L"sentinel.txt") << original;
    ok &= check(recoverHudColors(wrongTypeGame, false) == Result::recoveryRequired
                    && recoverHudColors(wrongTypeGame) == Result::recoveryRequired
                    && commitHudColors(wrongTypeGame, edited, {}, false) == Result::recoveryRequired
                    && !std::filesystem::exists(wrongTypeColors) && read(wrongTypeBackup / L"sentinel.txt") == original,
                "Startup and save must leave a directory at the recovery path intact and report it for review.");
    const auto retainedDirectory = wrongTypeBackup.parent_path() / L"retained-directory";
    ok &= check(MoveFileExW(wrongTypeBackup.c_str(), retainedDirectory.c_str(), MOVEFILE_WRITE_THROUGH)
                    && commitHudColors(wrongTypeGame, edited, {}, false) == Result::success
                    && read(wrongTypeColors) == edited && read(retainedDirectory / L"sentinel.txt") == original,
                "A normal color save must succeed once the unrelated directory has been moved aside.");

    interruption = Interruption::rollbackDidNotMove;
    injected = false;
    ok &= check(commitHudColors(game, edited, merged, true) == Result::conflict && injected
                    && read(colors) == external && !std::filesystem::exists(backup),
                "A partial rollback must put the first external edit back without losing it.");

    ok &= check(publishExternal(colors, "") && recoverHudColors(game) == Result::empty,
                "An unlocked empty file must be recognized without rewriting it.");
    const auto large = std::string(chrome_axis::detail::maxHudColorBytes + 1, 'a');
    ok &= check(publishExternal(colors, large) && recoverHudColors(game) == Result::tooLarge
                    && commitHudColors(game, edited, large, true) == Result::tooLarge && read(colors) == large,
                "An oversized file must be reported separately and preserved.");
    ok &= check(std::distance(std::filesystem::directory_iterator(directory),
                             std::filesystem::directory_iterator()) == 1,
                "A staged file must be cleaned up after success and after a rejected replacement.");

    ok &= check(publishExternal(colors, edited), "Unable to prepare the second concurrent replace.");
    interruption = Interruption::concurrentRestore;
    injected = false;
    ok &= check(commitHudColors(game, original, edited, true) == Result::conflict && injected
                    && read(colors) == later,
                "An update arriving during recovery must also be recovered.");
    bool preservedFirst{};
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().filename().wstring().starts_with(L"colors.json.recovery.saved."))
            preservedFirst |= read(file.path()) == external;
    }
    ok &= check(preservedFirst && recoverHudColors(game, false) == Result::success
                    && commitHudColors(game, later, later, true) == Result::success,
                "Keep an archived snapshot after successful rollback without blocking the next save.");

    interruption = Interruption::lockedDuringRollback;
    injected = false;
    ok &= check(commitHudColors(game, edited, later, true) == Result::recoveryRequired && injected,
                "An unreadable target during rollback must leave an explicit recovery state.");
    rollbackLock.reset();
    ok &= check(read(colors) == edited && read(backup) == external
                    && recoverHudColors(game) == Result::recoveryRequired,
                "A live file and a pending recovery file must both survive an ambiguous publication.");
    ok &= check(commitHudColors(game, original, edited, true) == Result::recoveryRequired
                    && read(colors) == edited && read(backup) == external,
                "A later save must not proceed silently while both ambiguous versions exist.");

    const auto legacyGame = game / L"legacy-0481";
    const auto legacyColors = chrome_axis::detail::hudColorFile(legacyGame);
    const auto legacyBackup = chrome_axis::detail::hudColorSidecar(legacyColors, L".recovery.tmp.123.456");
    const auto legacyCanonical = chrome_axis::detail::hudColorSidecar(legacyColors, L".recovery");
    ok &= check(commitHudColors(legacyGame, original, {}, false) == Result::success
                    && publishExternal(legacyBackup, later),
                "Unable to prepare a legacy .81 checkpoint.");
    ok &= check(recoverHudColors(legacyGame, false) == Result::recoveryRequired
                    && commitHudColors(legacyGame, edited, original, true) == Result::recoveryRequired
                    && read(legacyColors) == original && read(legacyBackup) == later,
                "Report a legacy .81 checkpoint even when its current colors are readable.");
    ok &= check(MoveFileExW(legacyColors.c_str(), legacyCanonical.c_str(), MOVEFILE_WRITE_THROUGH)
                    && recoverHudColors(legacyGame) == Result::recoveryRequired
                    && commitHudColors(legacyGame, edited, {}, false) == Result::recoveryRequired
                    && !std::filesystem::exists(legacyColors) && read(legacyCanonical) == original
                    && read(legacyBackup) == later,
                "Do not guess which .81 checkpoint to restore when the current file is missing.");

    for (const auto& [scenario, targetSurvives] : {std::pair{Interruption::interruptAfterRollback, true},
                                                   std::pair{Interruption::interruptDuringRollback, false}}) {
        const auto crashGame = game / (targetSurvives ? L"interrupted-restoration" : L"interrupted-displacement");
        const auto crashColors = chrome_axis::detail::hudColorFile(crashGame);
        ok &= check(commitHudColors(crashGame, original, {}, false) == Result::success,
                    "Unable to prepare an interrupted rollback fixture.");
        interruption = scenario;
        injected = false;
        bool interrupted{};
        try {
            (void)commitHudColors(crashGame, edited, original, true);
        } catch (const InterruptedSave&) {
            interrupted = true; // Reenter through the same probe and save entry points as the next launch.
        }
        bool latestSurvived{};
        for (const auto& file : std::filesystem::directory_iterator(crashColors.parent_path())) {
            if (file.path().filename().wstring().starts_with(L"colors.json.recovery.tmp."))
                latestSurvived |= read(file.path()) == later;
        }
        ok &= check(interrupted && injected && latestSurvived && std::filesystem::exists(crashColors) == targetSurvives
                        && !std::filesystem::exists(chrome_axis::detail::hudColorSidecar(crashColors, L".recovery"))
                        && recoverHudColors(crashGame, false) == Result::recoveryRequired
                        && commitHudColors(crashGame, merged, external, targetSurvives) == Result::recoveryRequired
                        && (targetSurvives ? read(crashColors) == external : !std::filesystem::exists(crashColors)),
                    "An interrupted second rollback must safeguard every snapshot even after consuming the canonical copy.");
    }
    return ok ? 0 : 1;
}
