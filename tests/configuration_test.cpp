#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"

#include <windows.h>

#include <fstream>
#include <iostream>

namespace {

bool check(bool condition, const char* description)
{
    if (!condition)
        std::cerr << description << '\n';
    return condition;
}

struct Fixture {
    std::filesystem::path directory;

    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

void createFile(const std::filesystem::path& path)
{
    std::ofstream file(path);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.close();
}

std::filesystem::path createPluginLayout(const std::filesystem::path& gameRoot)
{
    const auto plugin = gameRoot / L"red4ext" / L"plugins" / L"chrome_axis";
    std::filesystem::create_directories(plugin / L"scripts");
    createFile(plugin / L"ChromeAxis.dll");
    return plugin;
}

void createGameExecutable(const std::filesystem::path& gameRoot)
{
    const auto bin = gameRoot / L"bin" / L"x64";
    std::filesystem::create_directories(bin);
    createFile(bin / L"Cyberpunk2077.exe");
}

bool testDeploymentProfiles(const std::filesystem::path& fixture)
{
    const auto staging = createPluginLayout(fixture / L"staging");
    createFile(staging / L"scripts" / L"driving_context.reds");
    const auto stagedApp = staging / L"ChromeAxis.exe";
    createFile(stagedApp);
    const auto localProfile = chrome_axis::profileDirectory(stagedApp);
    if (!check(chrome_axis::installedGameRoot(staging).empty() &&
                   chrome_axis::calibrationProfileDirectory(stagedApp) == localProfile,
               "An app outside the game must retain its local profile"))
        return false;

    const auto incompleteGame = fixture / L"incomplete";
    const auto incomplete = createPluginLayout(incompleteGame);
    createGameExecutable(incompleteGame);
    const auto incompleteApp = incomplete / L"ChromeAxis.exe";
    const auto incompleteProfile = chrome_axis::profileDirectory(incompleteApp);
    const auto missingScript = chrome_axis::pluginFiles(incomplete);
    if (!check(chrome_axis::installedGameRoot(incomplete) == incompleteGame &&
                   chrome_axis::calibrationProfileDirectory(incompleteApp) == incompleteProfile &&
                   missingScript.library && !missingScript.script &&
                   !chrome_axis::isInstalledPluginDirectory(incomplete),
               "A missing script must keep the game identity and block activation"))
        return false;

    std::filesystem::remove(incomplete / L"ChromeAxis.dll");
    const auto missingBoth = chrome_axis::pluginFiles(incomplete);
    if (!check(!missingBoth.library && !missingBoth.script && !chrome_axis::isInstalledPluginDirectory(incomplete),
               "Diagnostics must detect both missing companion files"))
        return false;

    createFile(incomplete / L"scripts" / L"driving_context.reds");
    const auto missingLibrary = chrome_axis::pluginFiles(incomplete);
    if (!check(chrome_axis::installedGameRoot(incomplete) == incompleteGame &&
                   !missingLibrary.library && missingLibrary.script &&
                   !chrome_axis::isInstalledPluginDirectory(incomplete),
               "A missing DLL must keep the game identity and block activation"))
        return false;

    wchar_t volume[MAX_PATH]{};
    DWORD flags{};
    if (!check(GetVolumePathNameW(stagedApp.c_str(), volume, MAX_PATH) &&
                   GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0),
               "Could not query the hard link capability of the test volume"))
        return false;
    if ((flags & FILE_SUPPORTS_HARD_LINKS) == 0) {
        std::cout << "Skipping deployment aliases on a volume without hard link support\n";
        return true;
    }

    std::filesystem::create_hard_link(stagedApp, incompleteApp);
    const auto resolvedIncomplete = chrome_axis::calibrationProfileDirectory(stagedApp);
    if (!check(CompareStringOrdinal(resolvedIncomplete.c_str(), -1, incompleteProfile.c_str(), -1, TRUE) == CSTR_EQUAL,
               "A single incomplete deployment must resolve to the game for diagnostics"))
        return false;
    std::filesystem::remove(incompleteApp);

    const auto deployed = createPluginLayout(fixture / L"game one");
    createGameExecutable(fixture / L"game one");
    createFile(deployed / L"scripts" / L"driving_context.reds");
    const auto deployedApp = deployed / L"ChromeAxis.exe";
    std::filesystem::create_hard_link(stagedApp, deployedApp);
    const auto deployedProfile = chrome_axis::profileDirectory(deployedApp);
    const auto resolvedProfile = chrome_axis::calibrationProfileDirectory(stagedApp);
    if (!check(chrome_axis::isInstalledPluginDirectory(deployed) &&
                   CompareStringOrdinal(resolvedProfile.c_str(), -1, deployedProfile.c_str(), -1, TRUE) == CSTR_EQUAL,
               "A single valid hard link must resolve to the deployed profile") ||
        !check(!std::filesystem::exists(deployedProfile), "Resolving a profile must not create directories"))
        return false;

    std::filesystem::create_hard_link(stagedApp, incompleteApp);
    return check(chrome_axis::calibrationProfileDirectory(stagedApp) == localProfile,
                 "Multiple game deployments must retain the local profile even if one is incomplete") &&
        check(chrome_axis::calibrationProfileDirectory(deployedApp) == deployedProfile,
              "An app opened directly in the game must use that installation's profile");
}

bool testTemporaryProfiles()
{
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, buffer);
    if (length == 0 || length > MAX_PATH)
        return true;
    auto temporary = std::filesystem::path(buffer).lexically_normal();
    if (!temporary.has_filename())
        temporary = temporary.parent_path();
    if (!temporary.is_absolute() || temporary == temporary.root_path())
        return true;

    const auto sibling = temporary.parent_path() / (temporary.filename().wstring() + L"-work");
    return check(chrome_axis::isTemporaryProfileDirectory(temporary / L"7z-test" / L"Profil"),
                 "A profile opened in an archive extraction directory is temporary") &&
        check(!chrome_axis::isTemporaryProfileDirectory(sibling / L"Profil"),
              "A common path prefix must not mark a portable profile as temporary") &&
        check(!chrome_axis::isTemporaryProfileDirectory(temporary / L".." / sibling.filename() / L"Profil"),
              "Path normalization must resolve parent directory segments");
}

} // namespace

int main()
{
    const auto executable = chrome_axis::modulePath(GetModuleHandleW(nullptr));
    if (!check(executable.is_absolute(), "Expected an absolute executable path") ||
        !check(chrome_axis::profileDirectory(executable) == executable.parent_path() / L"Profil",
               "The profile directory must be beside the executable") ||
        !check(chrome_axis::profileDirectory(executable.parent_path() / L"ChromeAxis.dll") ==
                   chrome_axis::profileDirectory(executable), "The EXE and DLL must resolve the same profile directory") ||
        !check(chrome_axis::profileDirectory(L"ChromeAxis.dll").empty(),
               "A relative module path must not resolve against the working directory"))
        return 1;

    const auto directory = std::filesystem::current_path() /
        (L"chrome-axis-config-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!check(!std::filesystem::exists(directory), "Test directory already exists"))
        return 1;
    const Fixture fixture{directory};
    std::string error;
    if (!check(!chrome_axis::usbEnabled(directory), "USB input must be disabled without a marker") ||
        !check(chrome_axis::setUsbEnabled(directory, false, error), "Disable must be idempotent") ||
        !check(!std::filesystem::exists(directory), "Reading or disabling an absent marker must not write") ||
        !check(chrome_axis::setUsbEnabled(directory, true, error) && chrome_axis::usbEnabled(directory), "Enable must create an opt-in marker") ||
        !check(chrome_axis::setUsbEnabled(directory, true, error), "Enable must be idempotent"))
        return 1;

    const chrome_axis::Profile profile{
        {"{6F1D2B60-D5A0-11CF-BFC7-444553540000}", 0, 500, 0, 1000, 0.02f},
        {"{6F1D2B61-D5A0-11CF-BFC7-444553540000}", 1, 0, 0, 1000, 0.02f},
        {"{6F1D2B61-D5A0-11CF-BFC7-444553540000}", 2, 0, 0, 1000, 0.02f}
    };
    const auto profilePath = directory / chrome_axis::profileFilename;
    if (!check(chrome_axis::saveProfile(profilePath, profile, error), "Expected test calibration to save") ||
        !check(chrome_axis::setUsbEnabled(directory, false, error) && !chrome_axis::usbEnabled(directory), "Disable must remove the marker") ||
        !check(chrome_axis::loadProfile(profilePath, error).has_value(), "Disabling USB must preserve calibration"))
        return 1;

    const auto impostor = directory / L"usb.enabled";
    std::filesystem::create_directory(impostor);
    if (!check(!chrome_axis::usbEnabled(directory), "A directory is not an opt-in marker") ||
        !check(!chrome_axis::setUsbEnabled(directory, true, error), "Do not overwrite a non-file marker") ||
        !check(!chrome_axis::setUsbEnabled(directory, false, error), "Do not remove a directory on disable") ||
        !check(std::filesystem::is_directory(impostor), "Non-file marker must be preserved"))
        return 1;
    return testDeploymentProfiles(directory) && testTemporaryProfiles() ? 0 : 1;
}
