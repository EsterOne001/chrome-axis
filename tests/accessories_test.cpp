#include "chrome_axis/accessories.hpp"

#include <fstream>
#include <iostream>

namespace {

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
    std::filesystem::remove(path);
    if (!saved || !rejectedDuplicate || !rejectedZeroTravel || !rejectedDisallowedKey
        || !loaded || *loaded != profile || !savedEmpty || !empty || !empty->empty())
        return 1;

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
    handbrake.axesPresent[7] = true;
    if (reader.update(connected).any())
        return 10;
    reader.reset();
    if (reader.ready() || chrome_axis::AccessoryReader(*empty).ready())
        return 11;

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
