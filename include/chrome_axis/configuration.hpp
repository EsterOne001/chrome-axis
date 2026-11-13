#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace chrome_axis {

inline constexpr wchar_t usbMarkerFilename[] = L"usb.enabled";

// These lookups do not create directories or write files.
std::filesystem::path modulePath(HMODULE module);
std::filesystem::path profileDirectory(const std::filesystem::path& moduleFile);
// Identify a game installation even when a companion mod file is missing.
std::filesystem::path installedGameRoot(const std::filesystem::path& pluginDirectory);

struct PluginFiles {
    bool library{};
    bool script{};

    bool complete() const noexcept { return library && script; }
};

PluginFiles pluginFiles(const std::filesystem::path& directory);
bool isInstalledPluginDirectory(const std::filesystem::path& directory);
// The app can be launched through the staging link created by a mod manager.
std::filesystem::path calibrationProfileDirectory(const std::filesystem::path& appFile);
bool isTemporaryProfileDirectory(const std::filesystem::path& directory);
bool usbEnabled(const std::filesystem::path& directory) noexcept;

// The USB opt-in is independent of the calibration profile.
bool setUsbEnabled(const std::filesystem::path& directory, bool enabled, std::string& error);

} // namespace chrome_axis
