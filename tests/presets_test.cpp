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
        && check(selectPreset(root, name, error) && activePreset(root).name == name, "Explicit selection must repair corrupt selection") ? 0 : 1;
}
