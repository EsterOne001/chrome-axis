#pragma once

#include <filesystem>
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

} // namespace chrome_axis
