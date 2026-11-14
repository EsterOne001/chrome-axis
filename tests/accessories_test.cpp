#include "chrome_axis/accessories.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <utility>

namespace {

int testThresholds(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    const AccessoryBinding handbrake{deviceId, AccessoryKind::Axis, 7, 1000, 0, VK_SPACE};
    const AccessoryBinding button{deviceId, AccessoryKind::Button, 0, 0, 1, VK_ADD};
    auto adjusted = handbrake;
    adjusted.thresholds = {45, 20};
    const AccessoryProfile profile{adjusted, button};
    std::string error;
    const auto version = [&] {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        std::getline(file, line);
        return line;
    };
    if (!saveAccessoryProfile(path, profile, error) || version() != "version=2"
        || loadAccessoryProfile(path, error) != profile)
        return 21;
    auto invalid = profile;
    for (const AccessoryThresholds thresholds : {AccessoryThresholds{0, 0}, AccessoryThresholds{101, 20},
                                                AccessoryThresholds{25, 25}, AccessoryThresholds{25, 60}}) {
        invalid.front().thresholds = thresholds;
        if (saveAccessoryProfile(path, invalid, error))
            return 22;
    }
    invalid = profile;
    invalid.back().thresholds = adjusted.thresholds;
    if (saveAccessoryProfile(path, invalid, error) || loadAccessoryProfile(path, error) != profile)
        return 23;

    for (const auto& [number, row] : std::array<std::pair<int, const char*>, 4>{{
             {1, ",axis,7,1000,0,32,45,20"},
             {2, ",axis,7,1000,0,32,45"},
             {2, ",axis,7,1000,0,32,45,20,1"},
             {2, ",button,0,0,1,32,45,20"},
         }}) {
        std::ofstream file(path, std::ios::trunc);
        file << "version=" << number << "\nbinding=" << deviceId << row << '\n';
        file.close();
        if (loadAccessoryProfile(path, error))
            return 24;
    }
    if (!saveAccessoryProfile(path, {handbrake, button}, error) || version() != "version=1"
        || loadAccessoryProfile(path, error) != AccessoryProfile{handbrake, button})
        return 25;

    DeviceSample sample;
    sample.axesPresent[7] = true;
    const std::array<const DeviceSample*, 1> connected{&sample};
    AccessoryReader legacy({handbrake});
    AccessoryReader custom({adjusted});
    sample.axes[7] = 1000;
    for (int i = 0; i < 3; ++i) {
        legacy.update(connected);
        custom.update(connected);
    }
    struct Expected {
        long raw;
        bool legacy;
        bool custom;
    };
    for (const auto [raw, legacyDown, customDown] : std::array<Expected, 7>{{
             {551, false, false}, {550, false, true}, {450, true, true}, {650, true, true},
             {651, false, true}, {800, false, true}, {801, false, false},
         }}) {
        sample.axes[7] = raw;
        if (legacy.update(connected).test(VK_SPACE) != legacyDown ||
            custom.update(connected).test(VK_SPACE) != customDown)
            return 26;
    }

    adjusted.rest = 0;
    adjusted.engaged = 100;
    adjusted.thresholds = {8, 4};
    AccessoryReader low({adjusted});
    sample.axes[7] = 12;
    for (int i = 0; i < 4; ++i) {
        if (low.update(connected).any() || low.ready())
            return 27;
    }
    sample.axes[7] = 0;
    for (int i = 0; i < 3; ++i)
        low.update(connected);
    sample.axes[7] = 8;
    if (!low.ready() || !low.update(connected).test(VK_SPACE))
        return 28;
    sample.axes[7] = 4;
    if (!low.update(connected).test(VK_SPACE))
        return 29;
    sample.axes[7] = 3;
    return low.update(connected).any() ? 30 : 0;
}

int testShifterRoles(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    AccessoryProfile profile{
        {deviceId, AccessoryKind::Button, 0, 0, 1, VK_NUMPAD1, {}, ShifterRole::Reverse},
        {deviceId, AccessoryKind::Pov, 0, povCentered, 9000, 'E', {}, ShifterRole::Gear8},
        {deviceId, AccessoryKind::Axis, 2, 0, 100, VK_SUBTRACT, {60, 35}, ShifterRole::Downshift},
        {deviceId, AccessoryKind::Button, 1, 0, 1, VK_ADD, {}, ShifterRole::Upshift},
        {deviceId, AccessoryKind::Button, 2, 0, 1, VK_NUMPAD7},
    };
    std::string error;
    if (!saveAccessoryProfile(path, profile, error) || loadAccessoryProfile(path, error) != profile)
        return 41;

    auto edited = profile;
    edited.front().role = ShifterRole::Gear1;
    if (edited == profile || !sameAccessoryInputProfile(profile, edited)
        || sameAccessoryInputProfile(profile, std::nullopt))
        return 42;
    edited.front().key = 'G';
    if (sameAccessoryInputProfile(profile, edited)
        || !saveAccessoryProfile(path, edited, error) || loadAccessoryProfile(path, error) != edited)
        return 43;
    edited.front().role = static_cast<ShifterRole>(1000);
    if (saveAccessoryProfile(path, edited, error))
        return 44;

    // Older readers ignore metadata comments. Unrecognized or detached comments do not name a binding.
    std::ofstream file(path, std::ios::trunc);
    file << "# shifter-role=reverse\nversion=1\nbinding=" << deviceId << ",button,0,0,1,97\n"
         << "# shifter-role=gear-1\n\nbinding=" << deviceId << ",button,1,0,1,98\n"
         << "# shifter-role=gear-2\n# an unrelated comment\nbinding=" << deviceId << ",button,2,0,1,99\n"
         << "# shifter-role=gear-15\nbinding=" << deviceId << ",button,3,0,1,100\n"
         << "# shifter-role=gear-5\nbinding=" << deviceId << ",button,4,0,1,101\n"
         << "binding=" << deviceId << ",button,5,0,1,102\n";
    file.close();
    const auto restored = loadAccessoryProfile(path, error);
    const std::array expected{ShifterRole::None, ShifterRole::None, ShifterRole::None, ShifterRole::None,
                              ShifterRole::Gear5, ShifterRole::None};
    if (!restored || !std::ranges::equal(*restored, expected, std::ranges::equal_to{}, &AccessoryBinding::role))
        return 45;
    return 0;
}

int run()
{
    const std::string wheelId = "{11111111-1111-1111-1111-111111111111}";
    const std::string handbrakeId = "{22222222-2222-2222-2222-222222222222}";
    using chrome_axis::AccessoryKind;
    const chrome_axis::AccessoryProfile profile{
        {wheelId, AccessoryKind::Button, chrome_axis::buttonCount - 1, 0, 1, VK_UP},
        {wheelId, AccessoryKind::Pov, 0, chrome_axis::povCentered, 9000, VK_NUMPAD7},
        {handbrakeId, AccessoryKind::Axis, 7, 1000, 0, VK_SPACE},
        {wheelId, AccessoryKind::Button, 0, 0, 1, VK_ADD},
        {wheelId, AccessoryKind::Button, 1, 0, 1, VK_SUBTRACT},
    };
    const auto path = std::filesystem::current_path()
        / (L"accessories-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    const bool saved = chrome_axis::saveAccessoryProfile(path, profile, error);
    auto invalid = profile;
    invalid[1].key = invalid[0].key;
    const bool rejectedDuplicate = !chrome_axis::saveAccessoryProfile(path, invalid, error);
    invalid[1].key = profile[1].key;
    invalid[2].engaged = invalid[2].rest;
    const bool rejectedZeroTravel = !chrome_axis::saveAccessoryProfile(path, invalid, error);
    const auto loaded = chrome_axis::loadAccessoryProfile(path, error);
    {
        std::ofstream broken(path, std::ios::app);
        broken << "binding=" << wheelId << ",button,0,0,1," << VK_LWIN << '\n';
    }
    const bool rejectedDisallowedKey = !chrome_axis::loadAccessoryProfile(path, error);
    const bool savedEmpty = chrome_axis::saveAccessoryProfile(path, {}, error);
    const auto empty = chrome_axis::loadAccessoryProfile(path, error);
    const auto thresholdResult = testThresholds(path, handbrakeId);
    const auto roleResult = testShifterRoles(path, wheelId);
    std::filesystem::remove(path);
    if (!saved || !rejectedDuplicate || !rejectedZeroTravel || !rejectedDisallowedKey
        || !loaded || *loaded != profile || !savedEmpty || !empty || !empty->empty())
        return 1;
    if (thresholdResult != 0)
        return thresholdResult;
    if (roleResult != 0)
        return roleResult;

    chrome_axis::AccessoryReader reader(*loaded);
    chrome_axis::DeviceSample wheel;
    chrome_axis::DeviceSample handbrake;
    wheel.buttonsPresent.set(chrome_axis::buttonCount - 1);
    wheel.buttonsPresent.set(0);
    wheel.buttonsPresent.set(1);
    wheel.povsPresent[0] = true;
    wheel.buttons.set(chrome_axis::buttonCount - 1);
    wheel.povs[0] = 9000;
    handbrake.axesPresent[7] = true;
    const std::array<const chrome_axis::DeviceSample*, 5> connected{&wheel, &wheel, &handbrake, &wheel, &wheel};
    for (int i = 0; i < 5; ++i) {
        if (reader.update(connected).any() || reader.ready())
            return 2;
    }
    wheel.buttons.reset();
    wheel.povs[0] = chrome_axis::povCentered;
    handbrake.axes[7] = 1000;
    for (int i = 0; i < 3; ++i) {
        if (reader.update(connected).any() || reader.ready() != (i == 2))
            return 3;
        const auto readiness = reader.readiness();
        // The two buttons left released during the initial checks are already armed.
        if (readiness.armed != (i == 2 ? profile.size() : 2) || readiness.deviceUnavailable || readiness.inputUnavailable)
            return 17;
    }

    wheel.buttons.set(chrome_axis::buttonCount - 1);
    wheel.buttons.set(0);
    wheel.buttons.set(1);
    wheel.povs[0] = 8900;
    handbrake.axes[7] = 400;
    auto keys = reader.update(connected);
    if (!keys.test(VK_UP) || !keys.test(VK_NUMPAD7) || !keys.test(VK_SPACE)
        || !keys.test(VK_ADD) || !keys.test(VK_SUBTRACT))
        return 4;

    handbrake.axes[7] = 550;
    const std::array<const chrome_axis::DeviceSample*, 5> disconnected{nullptr, nullptr, &handbrake, nullptr, nullptr};
    keys = reader.update(disconnected);
    if (keys.count() != 1 || !keys.test(VK_SPACE) || reader.ready())
        return 5;
    const auto disconnectedReadiness = reader.readiness();
    if (disconnectedReadiness.armed != 1 || !disconnectedReadiness.deviceUnavailable || disconnectedReadiness.inputUnavailable)
        return 18;

    handbrake.axes[7] = 700;
    if (reader.update(connected).any())
        return 6;
    handbrake.axes[7] = 550;
    if (reader.update(connected).any())
        return 7;
    handbrake.axes[7] = 400;
    if (!reader.update(connected).test(VK_SPACE))
        return 8;

    handbrake.axesPresent[7] = false;
    if (reader.update(connected).any())
        return 9;
    const auto missingInput = reader.readiness();
    if (missingInput.armed != 0 || missingInput.deviceUnavailable || !missingInput.inputUnavailable)
        return 19;
    handbrake.axesPresent[7] = true;
    if (reader.update(connected).any())
        return 10;
    reader.reset();
    if (reader.ready() || chrome_axis::AccessoryReader(*empty).ready())
        return 11;
    const auto afterReset = reader.readiness();
    if (afterReset.armed != 0 || afterReset.deviceUnavailable || afterReset.inputUnavailable)
        return 20;

    // A connected base can report a different layout after a driver or mode change.
    wheel.buttons.reset();
    wheel.povs[0] = chrome_axis::povCentered;
    wheel.buttonsPresent.reset(chrome_axis::buttonCount - 1);
    wheel.povsPresent[0] = false;
    handbrake.axes[7] = 1000;
    for (int i = 0; i < 3; ++i) {
        if (reader.update(connected).any() || reader.ready())
            return 12;
    }
    wheel.buttons.set(chrome_axis::buttonCount - 1);
    wheel.buttons.set(0);
    wheel.povs[0] = 9000;
    keys = reader.update(connected);
    if (keys.count() != 1 || !keys.test(VK_ADD))
        return 13;

    wheel.buttonsPresent.set(chrome_axis::buttonCount - 1);
    wheel.povsPresent[0] = true;
    if (reader.update(connected) != keys || reader.ready())
        return 14;
    wheel.buttons.reset();
    wheel.povs[0] = chrome_axis::povCentered;
    for (int i = 0; i < 3; ++i) {
        if (reader.update(connected).any() || reader.ready() != (i == 2))
            return 15;
    }
    wheel.buttons.set(chrome_axis::buttonCount - 1);
    wheel.povs[0] = 9000;
    keys = reader.update(connected);
    if (keys.count() != 2 || !keys.test(VK_UP) || !keys.test(VK_NUMPAD7))
        return 16;

    // A pending base report cannot replay a held shift, while a fresh handbrake still works.
    using Status = chrome_axis::DeviceReadStatus;
    const std::array pending{Status::pending, Status::pending, Status::available,
                             Status::pending, Status::pending};
    handbrake.axes[7] = 400;
    keys = reader.update(disconnected, pending);
    const auto pendingReadiness = reader.readiness();
    if (keys.count() != 1 || !keys.test(VK_SPACE) || pendingReadiness.armed != 1 ||
        !pendingReadiness.temporarilyUnavailable || pendingReadiness.deviceUnavailable)
        return 47;
    wheel.buttons.set(0); // A new press may also occur in an unknown interval.
    if (reader.update(connected) != keys || reader.ready())
        return 48;
    wheel.buttons.reset();
    wheel.povs[0] = chrome_axis::povCentered;
    if (reader.update(connected) != keys || !reader.ready())
        return 49;
    wheel.buttons.set(0);
    wheel.buttons.set(chrome_axis::buttonCount - 1);
    keys = reader.update(connected);
    if (keys.count() != 3 || !keys.test(VK_SPACE) || !keys.test(VK_ADD) || !keys.test(VK_UP))
        return 50;
    reader.update(disconnected, pending);
    const std::array resumed{Status::resumed, Status::resumed, Status::available,
                             Status::resumed, Status::resumed};
    keys = reader.update(connected, resumed);
    if (keys.count() != 1 || !keys.test(VK_SPACE) || reader.ready())
        return 51;
    wheel.buttons.reset();
    for (int i = 0; i < 3; ++i)
        reader.update(connected);
    if (!reader.ready())
        return 52;
    return 0;
}

} // namespace

int main()
{
    const int result = run();
    if (result != 0)
        std::cerr << "Accessory mapping regression, check " << result << '\n';
    return result;
}
