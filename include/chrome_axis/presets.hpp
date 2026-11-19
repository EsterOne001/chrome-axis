#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

struct ProfilePreset {
    std::wstring name;
    std::filesystem::path directory;
};

struct ProfileFileRevision {
    std::uint64_t volume{};
    std::uint64_t file{};
    std::uint64_t modified{};
    std::uint64_t size{};
    bool present{};

    bool operator==(const ProfileFileRevision&) const = default;
};

// A missing file has a revision of its own. A failed observation has no revision.
std::optional<ProfileFileRevision> profileFileRevision(const std::filesystem::path& path);

struct ProfileFileCheck {
    const wchar_t* filename;
    const std::optional<ProfileFileRevision>& revision;
};

enum class PresetSaveResult { saved, changed, ioError };

// Run the save with exclusive access to this installation's profile settings, if they are still current.
PresetSaveResult savePresetSettings(const std::filesystem::path& root, const ProfilePreset& expected,
                                    std::span<const ProfileFileCheck> checks, const std::function<bool()>& save);

// Serialize writes such as the USB marker, language and transmission shortcut with profile restoration.
bool saveInstallationSettings(const std::filesystem::path& root, const std::function<bool()>& save);

// The original profile has an empty name. An invalid selection has an empty directory.
ProfilePreset activePreset(const std::filesystem::path& root);
// A temporarily locked selection file is the only case with no result.
std::optional<ProfilePreset> readActivePreset(const std::filesystem::path& root);
std::vector<std::wstring> listPresets(const std::filesystem::path& root);
bool selectPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error);

// Copy the active profile's saved files, then select the new profile.
bool createPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error);

enum class DeviceReplacementError { none, invalidSelection, invalidProfile, deviceNotMapped, conflict, ioError, recoveryRequired };

// Replace saved assignments only in the expected active preset. On interrupted rollback,
// recoveryDirectory contains the original files that still need to be restored.
bool replacePresetDevice(const std::filesystem::path& root, const ProfilePreset& expected,
                         std::string_view oldId, std::string_view newId, DeviceReplacementError& error,
                         std::filesystem::path* recoveryDirectory = nullptr, bool* feedbackDisabled = nullptr);

inline constexpr wchar_t profileLanguageFilename[] = L"language.txt";
inline constexpr wchar_t profileBackupsFolder[] = L"Sauvegardes";

std::optional<std::string> readProfileLanguage(const std::filesystem::path& root, bool* locked = nullptr);
bool writeProfileLanguage(const std::filesystem::path& root, std::string_view language);

struct ProfileRestoreRecovery {
    bool pending{};
    std::filesystem::path backup;
    bool backupUsable{};
    bool backupLocked{};
    bool deviceReassignment{};
};

bool profileRestorePending(const std::filesystem::path& root);
ProfileRestoreRecovery readInterruptedProfileRestore(const std::filesystem::path& root);

enum class ProfileBackupError { none, invalidLocation, invalidBackup, destinationExists, ioError, recoveryRequired };

enum class ProfileBackupStep {
    none, lock, prepareBackup, copyProfile, finishBackup, prepareRestore, copyBackup, prepareRecovery, applyRestore
};

struct ProfileBackupFailure {
    ProfileBackupStep step{};
    std::uint32_t windowsError{};
    bool axisConflict{};
    bool dedicatedAccessoryAxisConflict{};
};

// The destination must be a new directory. A legacy language preference can be included in the export.
bool createProfileBackup(const std::filesystem::path& root, const std::filesystem::path& destination,
                         ProfileBackupError& error, std::string_view fallbackLanguage = {},
                         ProfileBackupFailure* failure = nullptr);
// Restore saved settings and keep the previous ones under root/Sauvegardes.
bool restoreProfileBackup(const std::filesystem::path& root, const std::filesystem::path& source,
                          ProfileBackupError& error, std::string_view fallbackLanguage = {},
                          std::filesystem::path* recoveryDirectory = nullptr,
                          ProfileBackupFailure* failure = nullptr);

} // namespace chrome_axis
