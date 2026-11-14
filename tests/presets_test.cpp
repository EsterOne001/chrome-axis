#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>

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

std::string readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool testStaleWrites(const std::filesystem::path& fixture)
{
    using namespace chrome_axis;
    const auto parent = fixture / L"Concurrent";
    const auto root = parent / L"Profil";
    std::filesystem::create_directory(parent);
    const auto preset = activePreset(root);
    const auto axesFile = root / profileFilename;
    const auto accessoriesFile = root / accessoryFilename;
    const auto feedbackFile = root / feedbackFilename;
    auto axesBaseline = profileFileRevision(axesFile);
    auto accessoriesBaseline = profileFileRevision(accessoriesFile);
    auto feedbackBaseline = profileFileRevision(feedbackFile);
    const std::array axesCheck{ProfileFileCheck{profileFilename, axesBaseline}};
    const std::array accessoriesCheck{ProfileFileCheck{accessoryFilename, accessoriesBaseline}};
    const std::array feedbackChecks{ProfileFileCheck{profileFilename, axesBaseline},
                                   ProfileFileCheck{feedbackFilename, feedbackBaseline}};
    const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
    const Profile calibration{{wheel, 0, 500, 0, 1000, 0.0f}};
    const AccessoryProfile bindings{{wheel, AccessoryKind::Axis, 1, 0, 1000, VK_F8, {70, 35}}};
    const FeedbackSettings feedback{true, 14, false, true};
    std::string error;
    bool preventSaveCalled = false;
    const auto shouldNotSave = [&] { preventSaveCalled = true; return true; };

    if (!check(axesBaseline && !axesBaseline->present && preset.directory == root,
               "Missing settings in a new Profil folder need a valid revision")
        || !check(savePresetSettings(root, preset, axesCheck, [&] {
                      std::filesystem::create_directory(root);
                      return saveProfile(axesFile, calibration, error);
                  }) == PresetSaveResult::saved, "A fresh calibration may create the Profil directory")
        || !check(savePresetSettings(root, preset, axesCheck, shouldNotSave) == PresetSaveResult::changed,
                  "A stale calibration must not overwrite the new calibration")
        || !check(savePresetSettings(root, preset, feedbackChecks, shouldNotSave) == PresetSaveResult::changed,
                  "A force feedback dialog for an earlier calibration must reject saving"))
        return false;

    axesBaseline = profileFileRevision(axesFile);
    if (!check(savePresetSettings(root, preset, accessoriesCheck, [&] {
                      return saveAccessoryProfile(accessoriesFile, bindings, error);
                  }) == PresetSaveResult::saved
                  && savePresetSettings(root, preset, accessoriesCheck, shouldNotSave) == PresetSaveResult::changed
                  && loadAccessoryProfile(accessoriesFile, error) == bindings,
               "A stale accessory dialog must not overwrite new bindings or thresholds")
        || !check(savePresetSettings(root, preset, feedbackChecks, [&] {
                      return saveFeedbackSettings(feedbackFile, feedback, error);
                  }) == PresetSaveResult::saved
                  && savePresetSettings(root, preset, feedbackChecks, shouldNotSave) == PresetSaveResult::changed
                  && loadFeedbackSettings(feedbackFile, error) == feedback,
               "A stale force feedback dialog must not overwrite new settings"))
        return false;

    accessoriesBaseline = profileFileRevision(accessoriesFile);
    if (!check(createPreset(root, L"Pilote", error)
                  && savePresetSettings(root, preset, accessoriesCheck, shouldNotSave) == PresetSaveResult::changed,
               "A save for a preset that is no longer active must be rejected")
        || !check(selectPreset(root, L"", error)
                  && savePresetSettings(root, preset, accessoriesCheck, [&] {
                      return savePresetSettings(root, preset, accessoriesCheck, shouldNotSave) == PresetSaveResult::ioError
                          && !createPreset(root, L"Verrouillé", error) && !selectPreset(root, L"Pilote", error);
                  }) == PresetSaveResult::saved,
               "Preset writes, selection and copies share the same lock"))
        return false;
    return check(!preventSaveCalled, "A rejected save must have no side effects");
}

bool testDeviceReplacement(const std::filesystem::path& fixture)
{
    using namespace chrome_axis;
    using Error = DeviceReplacementError;
    const std::string oldWheel = "{11111111-1111-1111-1111-111111111111}";
    const std::string newWheel = "{22222222-2222-2222-2222-222222222222}";
    const std::string other = "{33333333-3333-3333-3333-333333333333}";
    const auto root = fixture / L"Replacement";
    std::filesystem::create_directory(root);
    const Profile calibration{{oldWheel, 0, 500, 0, 1000, 0.0f}, {newWheel, 1, 0, 0, 900, 0.03f},
        {newWheel, 2, 0, 0, 900, 0.02f}, {50, 80}};
    const AccessoryProfile bindings{{oldWheel, AccessoryKind::Button, 3, 0, 1, VK_ADD},
        {oldWheel, AccessoryKind::Pov, 0, povCentered, 0, VK_SUBTRACT},
        {newWheel, AccessoryKind::Pov, 0, povCentered, 18000, VK_SPACE},
        {oldWheel, AccessoryKind::Axis, 3, 0, 1000, VK_F8, {65, 30}}};
    const FeedbackSettings feedback{true, 19, false, true};
    std::string details;
    if (!check(saveProfile(root / profileFilename, calibration, details)
               && saveAccessoryProfile(root / accessoryFilename, bindings, details)
               && saveFeedbackSettings(root / feedbackFilename, feedback, details)
               && createPreset(root, L"Pilote", details) && setUsbEnabled(root, true, details)
               && writeProfileLanguage(root, "fr"), "Could not seed device-replacement fixture"))
        return false;

    const auto selected = activePreset(root);
    const auto analog = selected.directory / profileFilename;
    const auto digital = selected.directory / accessoryFilename;
    const auto note = selected.directory / L"Unrelated.txt";
    std::ofstream(note, std::ios::binary) << "preserve\r\n";
    std::ofstream(analog, std::ios::binary | std::ios::app) << "# " << oldWheel << "\r\nfuture.value = " << oldWheel;
    std::ofstream(digital, std::ios::binary | std::ios::app) << "# " << oldWheel << "\r\n";
    const auto originalAnalog = readBytes(analog);
    const auto originalDigital = readBytes(digital);
    const auto untouched = [&] {
        return readBytes(analog) == originalAnalog && readBytes(digital) == originalDigital;
    };

    Error error{};
    auto recovery = fixture;
    if (!check(!replacePresetDevice(root, selected, other, newWheel, error, &recovery)
               && error == Error::deviceNotMapped && recovery.empty() && untouched(),
               "Unknown source device must not modify the profile")
        || !check(selectPreset(root, L"", details) && !replacePresetDevice(root, selected, oldWheel, newWheel, error)
                  && error == Error::invalidSelection && untouched() && selectPreset(root, selected.name, details),
                  "A replacement must apply to the expected active profile only"))
        return false;

    const HANDLE locked = CreateFileW(digital.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!check(locked != INVALID_HANDLE_VALUE, "Could not hold the accessory settings open"))
        return false;
    const bool replacedWhileLocked = replacePresetDevice(root, selected, oldWheel, newWheel, error, &recovery);
    CloseHandle(locked);
    if (!check(!replacedWhileLocked && error == Error::ioError && recovery.empty() && untouched(),
               "An open accessory file must roll back the calibration replacement"))
        return false;

    const auto replaceAfter = [&](std::string& bytes, std::string_view prefix) {
        const auto offset = bytes.find(std::string(prefix) + oldWheel);
        if (offset != bytes.npos)
            bytes.replace(offset + prefix.size(), oldWheel.size(), newWheel);
    };
    auto expectedAnalog = originalAnalog;
    auto expectedDigital = originalDigital;
    replaceAfter(expectedAnalog, "steering.device=");
    replaceAfter(expectedDigital, "binding=");
    replaceAfter(expectedDigital, "binding=");
    replaceAfter(expectedDigital, "binding=");
    if (!check(replacePresetDevice(root, selected, oldWheel, newWheel, error, &recovery)
               && error == Error::none && recovery.empty() && readBytes(analog) == expectedAnalog
               && readBytes(digital) == expectedDigital && readBytes(note) == "preserve\r\n"
               && activePreset(root).name == selected.name && usbEnabled(root) && readProfileLanguage(root) == "fr"
               && loadFeedbackSettings(selected.directory / feedbackFilename, details) == feedback
               && loadAccessoryProfile(root / accessoryFilename, details) == bindings,
               "Only exact device tokens in the active profile may change")) {
        std::cerr << "Replacement error: " << static_cast<int>(error) << '\n';
        return false;
    }

    auto conflictingAxes = calibration;
    conflictingAxes.throttle.axis = 0;
    if (!check(saveProfile(analog, conflictingAxes, details) && saveAccessoryProfile(digital, bindings, details)
               && !replacePresetDevice(root, selected, oldWheel, newWheel, error) && error == Error::conflict,
               "Merging two devices must not place pedals on the steering axis"))
        return false;
    auto conflictingDigital = bindings;
    conflictingDigital.push_back({newWheel, AccessoryKind::Button, 3, 0, 1, VK_F6});
    if (!check(saveProfile(analog, calibration, details) && saveAccessoryProfile(digital, conflictingDigital, details)
               && !replacePresetDevice(root, selected, oldWheel, newWheel, error) && error == Error::conflict
               && loadAccessoryProfile(digital, details) == conflictingDigital,
               "Merging two devices must not put two commands on one button"))
        return false;

    std::filesystem::remove(analog);
    std::filesystem::remove(digital);
    conflictingDigital = {{oldWheel, AccessoryKind::Button, 5, 0, 1, VK_ADD}};
    if (!check(saveAccessoryProfile(digital, conflictingDigital, details)
               && replacePresetDevice(root, selected, oldWheel, newWheel, error)
               && !std::filesystem::exists(analog), "Accessory-only presets must stay accessory-only"))
        return false;
    conflictingDigital[0].deviceId = newWheel;
    return check(loadAccessoryProfile(digital, details) == conflictingDigital, "Accessory-only binding did not change");
}

bool testProfileBackups(const std::filesystem::path& fixture)
{
    using namespace chrome_axis;
    const auto root = fixture / L"Profil";
    const auto backup = fixture / L"Sauvegarde";
    const std::wstring name = L"Pédales été";
    const auto named = root / L"Profils" / name;
    const std::string wheel = "{11111111-1111-1111-1111-111111111111}";
    const Profile calibration{{wheel, 0, 500, 0, 1000, 0.0f}, {},
        {wheel, 2, 0, 0, 1000, 0.03f}, {-40, 75}};
    const AccessoryProfile bindings{{wheel, AccessoryKind::Button, 1, 0, 1, VK_ADD},
        {wheel, AccessoryKind::Axis, 3, 0, 1000, VK_SPACE, {40, 20}}};
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
    const auto wrongFolder = fixture / L"Sans sauvegarde" / L"Profil";
    std::filesystem::create_directories(wrongFolder);
    std::filesystem::path recovery;
    const auto rejectWrongFolder = [&] {
        const bool rejected = !restoreProfileBackup(root, wrongFolder, backupError, {}, &recovery);
        const auto active = loadProfile(named / profileFilename, error);
        return rejected && backupError == ProfileBackupError::invalidBackup && recovery.empty()
            && !std::filesystem::exists(root / profileBackupsFolder)
            && activePreset(root).name == name && usbEnabled(root)
            && active && active->steering.deadzone == calibration.steering.deadzone
            && loadAccessoryProfile(named / accessoryFilename, error) == bindings
            && loadFeedbackSettings(named / feedbackFilename, error) == feedback;
    };
    if (!check(rejectWrongFolder(),
               "An empty folder called Profil must be rejected"))
        return false;
    std::filesystem::create_directories(wrongFolder / L"Profils" / L"Sans réglages");
    std::ofstream(wrongFolder / L"Lisez-moi.txt") << "unrelated";
    if (!check(rejectWrongFolder(),
               "An unrelated folder with an empty named subfolder must leave live settings intact"))
        return false;

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

    if (!check(restoreProfileBackup(root, backup, backupError, {}, &recovery)
               && backupError == ProfileBackupError::none && recovery.parent_path() == root / profileBackupsFolder,
               "Restore must report its local recovery folder")) {
        std::cerr << "Restore error: " << static_cast<int>(backupError) << '\n';
        return false;
    }
    const auto axes = loadProfile(named / profileFilename, error);
    if (!check(activePreset(root).name == name && listPresets(root) == std::vector{name}
               && axes && axes->steering.deadzone == calibration.steering.deadzone
               && axes->brakeResponse == calibration.brakeResponse
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

    const auto selectedEmpty = fixture / L"Profil sélectionné";
    constexpr wchar_t selectedEmptyName[] = L"Sans réglages";
    std::filesystem::create_directories(selectedEmpty / L"Profils" / selectedEmptyName);
    if (!check(selectPreset(selectedEmpty, selectedEmptyName, error)
               && restoreProfileBackup(root, selectedEmpty, backupError)
               && activePreset(root).name == selectedEmptyName,
               "A manually copied empty named profile with an explicit selection must still restore"))
        return false;

    const auto namedOnly = fixture / L"Profils copiés";
    const auto namedOnlySettings = namedOnly / L"Profils" / name;
    std::filesystem::create_directories(namedOnlySettings);
    if (!check(saveProfile(namedOnlySettings / profileFilename, calibration, error)
               && restoreProfileBackup(root, namedOnly, backupError)
               && activePreset(root).name.empty() && listPresets(root) == std::vector{name},
               "Recognized settings in a named profile must support importing a manual backup"))
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
        && testProfileBackups(root) && testDeviceReplacement(root) && testStaleWrites(root) ? 0 : 1;
}
