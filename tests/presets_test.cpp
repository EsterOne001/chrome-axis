#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <fstream>
#include <iostream>

namespace {

struct Fixture {
    std::filesystem::path directory;

    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

bool check(bool condition, const char* description)
{
    if (!condition)
        std::cerr << description << '\n';
    return condition;
}

bool testProfileBackups(const std::filesystem::path& fixture)
{
    using namespace chrome_axis;
    const auto root = fixture / L"Profil";
    const auto backup = fixture / L"Sauvegarde";
    const std::wstring name = L"Pédales été";
    const auto named = root / L"Profils" / name;
    const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
    const Profile calibration{{wheel, 0, 500, 0, 1000, 0.04f}};
    const AccessoryProfile bindings{{wheel, AccessoryKind::Button, 1, 0, 1, VK_ADD}};
    const FeedbackSettings feedback{true, 17, false, true};
    std::string error;
    ProfileBackupError backupError{};
    if (!check(std::filesystem::create_directory(root), "Could not set up a profile folder")
        || !check(saveProfile(root / profileFilename, calibration, error)
                  && saveAccessoryProfile(root / accessoryFilename, bindings, error)
                  && saveFeedbackSettings(root / feedbackFilename, feedback, error)
                  && setUsbEnabled(root, true, error) && createPreset(root, name, error),
                  "Could not seed all profile settings"))
        return false;
    std::ofstream(root / L"Lisez-moi.txt") << "deployed";
    std::filesystem::create_directories(root / profileBackupsFolder / L"Older");
    if (!check(createProfileBackup(root, backup, backupError, "fr")
               && readProfileLanguage(backup) == "fr" && !readProfileLanguage(root),
               "Export must include the fallback language without modifying the original")
        || !check(!std::filesystem::exists(backup / L"Lisez-moi.txt")
                  && !std::filesystem::exists(backup / profileBackupsFolder),
                  "Deployed files and older backups must not be copied")
        || !check(!createProfileBackup(root, backup, backupError)
                  && backupError == ProfileBackupError::destinationExists
                  && !createProfileBackup(root, root / L"Profils" / L"Invalid", backupError)
                  && backupError == ProfileBackupError::invalidLocation,
                  "Backups must not replace existing folders or create a live profile"))
        return false;

    if (!check(setUsbEnabled(root, false, error) && writeProfileLanguage(root, "en")
               && selectPreset(root, L"", error) && saveAccessoryProfile(root / accessoryFilename, {}, error)
               && createPreset(root, L"Plus récent", error), "Could not update the live profiles"))
        return false;
    const auto corrupt = fixture / L"Corrompu";
    std::filesystem::create_directory(corrupt);
    std::ofstream(corrupt / L"active_profile.txt") << "../elsewhere\n";
    if (!check(!restoreProfileBackup(root, corrupt, backupError) && backupError == ProfileBackupError::invalidBackup
               && activePreset(root).name == L"Plus récent" && readProfileLanguage(root) == "en",
               "An invalid backup must not change live settings"))
        return false;
    if (!check(writeProfileLanguage(corrupt, "fr"), "Could not prepare an unrelated folder"))
        return false;
    std::filesystem::remove(corrupt / L"active_profile.txt");
    if (!check(!restoreProfileBackup(root, corrupt, backupError) && backupError == ProfileBackupError::invalidBackup,
               "A folder containing only a language preference must not clear the profiles"))
        return false;

    const auto locked = CreateFileW((root / profileFilename).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!check(locked != INVALID_HANDLE_VALUE, "Could not hold the live calibration file open"))
        return false;
    const bool restoredWhileLocked = restoreProfileBackup(root, backup, backupError);
    CloseHandle(locked);
    if (!check(!restoredWhileLocked && backupError == ProfileBackupError::ioError
               && activePreset(root).name == L"Plus récent" && readProfileLanguage(root) == "en"
               && !usbEnabled(root) && loadAccessoryProfile(root / accessoryFilename, error) == AccessoryProfile{},
               "A blocked file must roll back all settings moved before the failure"))
        return false;

    std::filesystem::path recovery;
    if (!check(restoreProfileBackup(root, backup, backupError, {}, &recovery)
               && backupError == ProfileBackupError::none && recovery.parent_path() == root / profileBackupsFolder,
               "Restore must report its local recovery folder")) {
        std::cerr << "Restore error: " << static_cast<int>(backupError) << '\n';
        return false;
    }
    const auto axes = loadProfile(named / profileFilename, error);
    if (!check(activePreset(root).name == name && listPresets(root) == std::vector{name}
               && axes && axes->steering.deadzone == calibration.steering.deadzone
               && loadAccessoryProfile(root / accessoryFilename, error) == bindings
               && loadAccessoryProfile(named / accessoryFilename, error) == bindings
               && loadFeedbackSettings(named / feedbackFilename, error) == feedback
               && usbEnabled(root) && readProfileLanguage(root) == "fr",
               "Restore must apply the default and named settings, selection, USB and language")
        || !check(std::filesystem::exists(root / L"Lisez-moi.txt")
                  && std::filesystem::is_directory(root / profileBackupsFolder / L"Older")
                  && activePreset(recovery).name == L"Plus récent"
                  && readProfileLanguage(recovery) == "en" && !usbEnabled(recovery),
                  "Restore must retain unrelated live files and the previous settings"))
        return false;

    const auto legacy = fixture / L"Ancien";
    std::filesystem::create_directory(legacy);
    if (!check(saveProfile(legacy / profileFilename, calibration, error)
               && restoreProfileBackup(root, legacy, backupError)
               && activePreset(root).name.empty() && listPresets(root).empty() && !usbEnabled(root)
               && !std::filesystem::exists(root / accessoryFilename)
               && !std::filesystem::exists(root / feedbackFilename) && readProfileLanguage(root) == "fr",
               "Manual legacy backups must restore absent settings while preserving language"))
        return false;

    // Keep the live profile below MAX_PATH while the automatic recovery file exceeds it.
    const auto prefix = fixture / L"Chemins longs";
    const auto padded = prefix / std::wstring(prefix.native().size() < 154 ? 154 - prefix.native().size() : 1, L'x');
    const auto longRoot = padded / L"Profil";
    const auto longName = std::wstring(55, L'é');
    const auto longNamed = longRoot / L"Profils" / longName;
    std::filesystem::create_directories(longNamed);
    if (!check(saveProfile(longNamed / profileFilename, calibration, error)
               && selectPreset(longRoot, longName, error), "Could not seed the long profile")
        || !check(restoreProfileBackup(longRoot, legacy, backupError, {}, &recovery),
                  "A recovery path exceeding MAX_PATH must not block a restore"))
        return false;
    auto recoveredFile = recovery / L"Profils" / longName / profileFilename;
    if (!check(recoveredFile.native().size() >= MAX_PATH
               && std::filesystem::is_regular_file(LR"(\\?\)" + recoveredFile.make_preferred().native())
               && restoreProfileBackup(longRoot, recovery, backupError) && activePreset(longRoot).name == longName,
               "The long recovery path must remain usable as a restore source"))
        return false;

    const auto empty = fixture / L"Missing profile folder";
    const auto internal = root / profileBackupsFolder / L"Current";
    const auto emptyBackup = fixture / L"Empty";
    return check(createProfileBackup(root, internal, backupError)
                 && restoreProfileBackup(root, internal, backupError),
                 "Backups under the installed Profil directory must round-trip")
        && check(createProfileBackup(empty, emptyBackup, backupError)
                 && restoreProfileBackup(root, emptyBackup, backupError) && activePreset(root).name.empty()
                 && !std::filesystem::exists(root / profileFilename) && readProfileLanguage(root) == "fr",
                 "An empty snapshot must be valid and restore empty driving settings");
}

} // namespace

int main()
{
    using namespace chrome_axis;
    const auto root = std::filesystem::current_path() / (L"chrome-axis-presets-" + std::to_wstring(GetCurrentProcessId()));
    if (!check(std::filesystem::create_directory(root), "Expected a new test directory"))
        return 1;
    const Fixture fixture{root};

    const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
    const Profile calibration{{wheel, 0, 500, 0, 1000, 0.02f}};
    const AccessoryProfile bindings{{wheel, AccessoryKind::Button, 1, 0, 1, VK_ADD}};
    std::string error;
    if (!check(activePreset(root).directory == root && activePreset(root).name.empty(), "Legacy settings must remain active by default")
        || !check(saveProfile(root / profileFilename, calibration, error), "Could not save calibration")
        || !check(saveAccessoryProfile(root / accessoryFilename, bindings, error), "Could not save accessory bindings")
        || !check(setUsbEnabled(root, true, error), "Could not save the independent USB marker"))
        return 1;

    const FeedbackSettings feedback{true, 12, false, true};
    if (!check(saveFeedbackSettings(root / feedbackFilename, feedback, error), "Could not save force feedback"))
        return 1;
    const std::wstring name = L"Volant été";
    const auto source = root / accessoryFilename;
    const DWORD initial = GetFileAttributesW(source.c_str());
    if (!check(initial != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(source.c_str(), initial | FILE_ATTRIBUTE_READONLY),
               "Could not make the fixture read-only"))
        return 1;
    const bool created = createPreset(root, name, error);
    const bool originalReadOnly = (GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_READONLY) != 0;
    const bool restored = SetFileAttributesW(source.c_str(), initial) != FALSE;
    if (!check(created && originalReadOnly && restored, "Creating a named copy must preserve the source attributes"))
        return 1;
    const auto copied = activePreset(root);
    const auto axes = loadProfile(copied.directory / profileFilename, error);
    const auto accessories = loadAccessoryProfile(copied.directory / accessoryFilename, error);
    if (!check(copied.name == name && copied.directory == root / L"Profils" / name, "Unicode selection did not round trip")
        || !check(axes && axes->steering.deviceId == wheel && accessories == bindings, "Named copy must carry saved axes and accessories")
        || !check(loadFeedbackSettings(copied.directory / feedbackFilename, error) == feedback, "Named copy must carry saved force feedback")
        || !check(usbEnabled(root) && !usbEnabled(copied.directory), "USB marker must stay global"))
        return 1;

    if (!check(!createPreset(root, L"VOLANT ÉTÉ", error), "Case variant must not replace an existing profile")
        || !check(!createPreset(root, L"..\\other", error) && !selectPreset(root, L"..\\other", error), "Paths are not valid preset names")
        || !check(!selectPreset(root, L"Absent", error) && activePreset(root).name == name, "Failed selection must preserve the active profile")
        || !check(selectPreset(root, L"", error) && activePreset(root).directory == root, "User must be able to reselect legacy settings")
        || !check(saveAccessoryProfile(copied.directory / accessoryFilename, {}, error), "Could not edit the named copy")
        || !check(loadAccessoryProfile(root / accessoryFilename, error) == bindings, "Editing named settings must not alter legacy settings"))
        return 1;

    std::filesystem::remove(root / L"force_feedback.ini");
    std::filesystem::create_directory(root / L"force_feedback.ini");
    if (!check(!createPreset(root, L"Copie incomplète", error), "Incomplete copies must fail")
        || !check(activePreset(root).directory == root && listPresets(root) == std::vector{name}, "Incomplete copies must not be listed or activated"))
        return 1;

    std::ofstream(root / L"active_profile.txt", std::ios::trunc) << "../other\n";
    return check(activePreset(root).directory.empty(), "Corrupt selection must not activate a different profile")
        && check(selectPreset(root, name, error) && activePreset(root).name == name, "Explicit selection must repair corrupt selection")
        && testProfileBackups(root) ? 0 : 1;
}
