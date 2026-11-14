#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

struct ProfilePreset {
    std::wstring name;
    std::filesystem::path directory;
};

// The original profile has an empty name. An invalid selection has an empty directory.
ProfilePreset activePreset(const std::filesystem::path& root);
std::vector<std::wstring> listPresets(const std::filesystem::path& root);
bool selectPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error);

// Copy the active profile's saved files, then select the new profile.
bool createPreset(const std::filesystem::path& root, std::wstring_view name, std::string& error);

enum class DeviceReplacementError { none, invalidSelection, invalidProfile, deviceNotMapped, conflict, ioError, recoveryRequired };

// Replace saved assignments only in the expected active preset. On interrupted rollback,
// recoveryDirectory contains the original files that still need to be restored.
bool replacePresetDevice(const std::filesystem::path& root, const ProfilePreset& expected,
                         std::string_view oldId, std::string_view newId, DeviceReplacementError& error,
                         std::filesystem::path* recoveryDirectory = nullptr);

inline constexpr wchar_t profileLanguageFilename[] = L"language.txt";
inline constexpr wchar_t profileBackupsFolder[] = L"Sauvegardes";

std::optional<std::string> readProfileLanguage(const std::filesystem::path& root);
bool writeProfileLanguage(const std::filesystem::path& root, std::string_view language);

enum class ProfileBackupError { none, invalidLocation, invalidBackup, destinationExists, ioError, recoveryRequired };

// The destination must be a new directory. A legacy language preference can be included in the export.
bool createProfileBackup(const std::filesystem::path& root, const std::filesystem::path& destination,
                         ProfileBackupError& error, std::string_view fallbackLanguage = {});
// Restore saved settings and keep the previous ones under root/Sauvegardes.
bool restoreProfileBackup(const std::filesystem::path& root, const std::filesystem::path& source,
                          ProfileBackupError& error, std::string_view fallbackLanguage = {},
                          std::filesystem::path* recoveryDirectory = nullptr);

} // namespace chrome_axis
