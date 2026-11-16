#include "chrome_axis/accessories.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
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

int testPreviewReadings(const std::string& deviceId)
{
    using namespace chrome_axis;
    AccessoryReader reader({
        {deviceId, AccessoryKind::Axis, 2, 1000, 0, VK_SPACE, {60, 35}},
        {deviceId, AccessoryKind::Button, 0, 0, 1, 'G', {}, ShifterRole::Gear1},
    });
    DeviceSample sample;
    sample.axesPresent[2] = true;
    sample.buttonsPresent.set(0);
    const std::array<const DeviceSample*, 2> connected{&sample, &sample};
    std::array<AccessoryObservation, 2> observations{};
    sample.axes[2] = 300;
    if (reader.update(connected, {}, observations).any() || observations[0].travel != 0.7
        || observations[0].status != AccessoryStatus::waitingForNeutral || observations[0].active)
        return 53;
    sample.axes[2] = 1000;
    for (int i = 0; i < 3; ++i)
        reader.update(connected, {}, observations);
    if (observations[0].status != AccessoryStatus::armed || observations[0].travel != 0.0
        || observations[1].status != AccessoryStatus::armed || observations[1].travel)
        return 54;
    sample.axes[2] = 400;
    sample.buttons.set(0);
    const auto keys = reader.update(connected, {}, observations);
    if (!keys.test(VK_SPACE) || !keys.test('G') || !observations[0].active || !observations[1].active
        || observations[0].travel != 0.6 || observations[1].travel)
        return 55;
    sample.axes[2] = 600;
    if (!reader.update(connected, {}, observations).test(VK_SPACE) || !observations[0].active)
        return 56;
    const std::array<const DeviceSample*, 2> disconnected{};
    reader.update(disconnected, {}, observations);
    if (observations[0].status != AccessoryStatus::deviceUnavailable || observations[0].active
        || observations[0].travel || observations[1].active)
        return 57;
    return 0;
}

int testProfileReplacement(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    const AccessoryProfile original{{deviceId, AccessoryKind::Button, 0, 0, 1, VK_ADD}};
    auto updated = original;
    updated.front().activation = AccessoryActivation::pulse;
    std::string error;
    if (!saveAccessoryProfile(path, original, error))
        return 88;

    const auto openWithoutDeleteSharing = [&] {
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return std::unique_ptr<void, decltype(&CloseHandle)>(handle == INVALID_HANDLE_VALUE ? nullptr : handle,
                                                            &CloseHandle);
    };
    auto reader = openWithoutDeleteSharing();
    if (!reader)
        return 89;
    {
        std::jthread closeReader([heldFile = std::move(reader)]() mutable {
            Sleep(50);
            heldFile.reset();
        });
        if (!saveAccessoryProfile(path, updated, error) || loadAccessoryProfile(path, error) != updated)
            return 90;
    }

    reader = openWithoutDeleteSharing();
    if (!reader)
        return 91;
    if (saveAccessoryProfile(path, original, error) || loadAccessoryProfile(path, error) != updated)
        return 92;
    reader.reset();
    if (!saveAccessoryProfile(path, original, error) || loadAccessoryProfile(path, error) != original)
        return 93;
    return 0;
}

int testPulseBindings(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    using Status = DeviceReadStatus;
    constexpr auto pulse = AccessoryActivation::pulse;
    const AccessoryProfile profile{
        {deviceId, AccessoryKind::Button, 0, 0, 1, VK_ADD, {}, ShifterRole::Upshift, pulse},
        {deviceId, AccessoryKind::Axis, 2, 0, 100, VK_SUBTRACT, {60, 25}, ShifterRole::Downshift, pulse},
        {deviceId, AccessoryKind::Button, 1, 0, 1, VK_SPACE},
    };
    std::string error;
    if (!saveAccessoryProfile(path, profile, error) || loadAccessoryProfile(path, error) != profile)
        return 58;
    {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        std::getline(file, line);
        if (line != "version=3")
            return 59;
    }
    auto legacy = profile;
    for (auto& binding : legacy)
        binding.activation = AccessoryActivation::hold;
    if (sameAccessoryInputProfile(profile, legacy) || !saveAccessoryProfile(path, legacy, error)
        || loadAccessoryProfile(path, error) != legacy)
        return 60;
    auto invalid = profile;
    invalid[0].activation = static_cast<AccessoryActivation>(99);
    if (saveAccessoryProfile(path, invalid, error))
        return 61;
    for (const auto& [number, mode] : std::array<std::pair<int, const char*>, 3>{{
             {1, "pulse"}, {2, "pulse"}, {3, "toggle"},
         }}) {
        std::ofstream file(path, std::ios::trunc);
        file << "version=" << number << "\nbinding=" << deviceId << ",button,0,0,1," << VK_ADD << ',' << mode << '\n';
        file.close();
        if (loadAccessoryProfile(path, error))
            return 62;
    }

    AccessoryReader reader(profile);
    DeviceSample sample;
    sample.buttonsPresent.set(0).set(1);
    sample.axesPresent[2] = true;
    const std::array<const DeviceSample*, 3> connected{&sample, &sample, &sample};
    const std::array<const DeviceSample*, 3> disconnected{};
    const std::array pending{Status::pending, Status::pending, Status::pending};
    const std::array resumed{Status::resumed, Status::resumed, Status::resumed};
    std::array<AccessoryObservation, 3> observations;
    std::bitset<256> impulses;
    auto update = [&](std::span<const DeviceSample* const> samples, std::span<const DeviceReadStatus> statuses = {}) {
        return reader.update(samples, statuses, observations, &impulses);
    };
    sample.buttons.set(0);
    sample.axes[2] = 60;
    impulses.set(VK_ADD); // Each update replaces the prior outputs.
    if (update(connected).any() || impulses.any() || observations[0].engaged != true
        || observations[1].engaged != true || observations[0].active || observations[1].active
        || observations[0].status != AccessoryStatus::waitingForNeutral || observations[0].key != VK_ADD
        || observations[1].key != VK_SUBTRACT || observations[2].key != VK_SPACE)
        return 63;
    const auto release = [&] {
        sample.buttons.reset();
        sample.axes[2] = 0;
        for (int i = 0; i < 3; ++i) {
            if (update(connected).any() || impulses.any())
                return false;
        }
        return reader.ready();
    };
    if (!release())
        return 64;
    const auto press = [&] {
        sample.buttons.set(0).set(1);
        sample.axes[2] = 60;
        const auto held = update(connected);
        return held.count() == 1 && held.test(VK_SPACE) && impulses.count() == 2
            && impulses.test(VK_ADD) && impulses.test(VK_SUBTRACT) && observations[0].active && observations[1].active;
    };
    if (!press() || !update(connected).test(VK_SPACE) || impulses.any())
        return 65;

    sample.axes[2] = 25;
    if (!update(connected).test(VK_SPACE) || impulses.any() || observations[1].engaged != true)
        return 66;
    sample.axes[2] = 24;
    if (!update(connected).test(VK_SPACE) || impulses.any() || observations[1].engaged != false)
        return 67;
    sample.axes[2] = 60;
    if (!update(connected).test(VK_SPACE) || impulses.count() != 1 || !impulses.test(VK_SUBTRACT))
        return 68;

    if (update(disconnected, pending).any() || impulses.any() || observations[0].engaged
        || observations[0].status != AccessoryStatus::temporarilyUnavailable
        || update(connected).any() || impulses.any())
        return 69;
    if (update(connected, resumed).any() || impulses.any() || !release() || !press()
        || update(disconnected).any() || impulses.any() || update(connected).any() || impulses.any())
        return 70;

    reader.reset();
    if (update(connected).any() || impulses.any() || !release() || !press())
        return 71;
    return 0;
}

int testProgressiveHandbrake(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    const AccessoryBinding handbrake{deviceId, AccessoryKind::Axis, 7, 1000, 0, 0, {}, ShifterRole::None,
                                    AccessoryActivation::hold, defaultHandbrakeCeilingPercent};
    const AccessoryBinding key{deviceId, AccessoryKind::Button, 0, 0, 1, VK_SPACE, {},
                               ShifterRole::None, AccessoryActivation::pulse};
    const AccessoryProfile profile{handbrake, key};
    std::string error;
    if (!saveAccessoryProfile(path, profile, error) || loadAccessoryProfile(path, error) != profile)
        return 72;
    std::ostringstream legacyContents;
    {
        std::ifstream current(path);
        std::string line;
        while (std::getline(current, line)) {
            if (!line.starts_with(progressiveHandbrakePrefix))
                legacyContents << line << '\n';
        }
    }
    // This is the legacy reader's view after it skips the unknown comment.
    if (legacyContents.str().find("version=3\n") == std::string::npos)
        return 73;
    std::ofstream(path, std::ios::trunc) << legacyContents.str();
    if (loadAccessoryProfile(path, error) != AccessoryProfile{key})
        return 74;

    auto changed = profile;
    changed.front().handbrakeCeilingPercent = 22;
    if (sameAccessoryInputProfile(profile, changed) || !saveAccessoryProfile(path, changed, error)
        || loadAccessoryProfile(path, error) != changed)
        return 75;
    auto invalid = profile;
    invalid.front().key = VK_F8;
    if (saveAccessoryProfile(path, invalid, error) || saveAccessoryProfile(path, {handbrake, handbrake}, error))
        return 76;
    for (const auto cap : {"0", "101", "15,pulse", "15,"}) {
        std::ofstream(path, std::ios::trunc) << "version=3\n" << progressiveHandbrakePrefix
            << deviceId << ",axis,7,1000,0,0," << cap << '\n';
        if (loadAccessoryProfile(path, error))
            return 77;
    }

    AccessoryReader reader(profile);
    DeviceSample sample;
    sample.axesPresent[7] = true;
    sample.buttonsPresent.set(0);
    const std::array<const DeviceSample*, 2> connected{&sample, &sample};
    const std::array<const DeviceSample*, 2> lost{nullptr, &sample};
    const std::array pending{DeviceReadStatus::pending, DeviceReadStatus::available};
    const std::array resumed{DeviceReadStatus::resumed, DeviceReadStatus::available};
    std::optional<float> command;
    std::bitset<256> pulses;
    std::array<AccessoryObservation, 2> observations;
    const auto update = [&](std::span<const DeviceSample* const> samples,
                            std::span<const DeviceReadStatus> statuses = {}) {
        return reader.update(samples, statuses, observations, &pulses, &command);
    };
    sample.axes[7] = 950;
    for (int i = 0; i < 4; ++i) {
        if (update(connected).any() || command || observations[0].status != AccessoryStatus::waitingForNeutral)
            return 78;
    }
    sample.axes[7] = 990;
    for (int i = 0; i < 3; ++i)
        update(connected);
    if (!reader.ready() || command != 0.0f)
        return 79;

    sample.axes[7] = 500;
    sample.buttons.set(0);
    if (update(connected).any() || pulses.count() != 1 || !pulses.test(VK_SPACE)
        || !command || std::abs(*command - 0.15f * 0.47f / 0.97f) > 0.0001f
        || !observations[0].active || observations[0].travel != 0.5)
        return 80;
    sample.axes[7] = -100;
    if (update(connected).any() || pulses.any() || command != 0.15f)
        return 81;

    // A suppressed key injection rearms keyboard bindings independently of the held lever,
    // even when the key and the lever share a DirectInput device.
    reader.resetKeyboardBindings();
    if (update(connected).any() || pulses.any() || command != 0.15f
        || observations[0].status != AccessoryStatus::armed
        || observations[1].status != AccessoryStatus::waitingForNeutral
        || reader.readiness().armed != 1)
        return 86;
    sample.buttons.reset(0);
    for (int i = 0; i < 3; ++i)
        update(connected);
    sample.buttons.set(0);
    if (update(connected).any() || pulses.count() != 1 || !pulses.test(VK_SPACE) || command != 0.15f)
        return 87;

    if (update(lost, pending).any() || command || observations[0].status != AccessoryStatus::temporarilyUnavailable
        || update(connected).any() || command || observations[0].status != AccessoryStatus::waitingForNeutral
        || update(connected, resumed).any() || command)
        return 82;
    sample.axes[7] = 1000;
    for (int i = 0; i < 3; ++i)
        update(connected);
    if (!reader.ready() || command != 0.0f)
        return 83;
    sample.axes[7] = 0;
    if (update(connected).any() || pulses.any() || command != 0.15f
        || update(lost).any() || command || observations[0].status != AccessoryStatus::deviceUnavailable
        || update(connected).any() || command)
        return 84;
    sample.axesPresent[7] = false;
    if (update(connected).any() || command || observations[0].status != AccessoryStatus::inputUnavailable)
        return 85;
    return 0;
}

int testNativeShifterProfile(const std::filesystem::path& path, const std::string& deviceId)
{
    using namespace chrome_axis;
    const AccessoryProfile profile{
        {deviceId, AccessoryKind::Button, 0, 0, 1, 0, {}, ShifterRole::Gear1, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Axis, 1, 0, 1000, 0, {62, 35}, ShifterRole::Gear2, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Pov, 0, povCentered, 18000, 0, {}, ShifterRole::Reverse, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Button, 2, 0, 1, VK_NUMPAD6, {}, ShifterRole::Gear6},
        {deviceId, AccessoryKind::Button, 3, 0, 1, VK_ADD, {}, ShifterRole::Upshift, AccessoryActivation::pulse},
    };
    std::string error;
    if (!saveAccessoryProfile(path, profile, error) || loadAccessoryProfile(path, error) != profile)
        return 94;
    {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        std::getline(file, line);
        if (line != "version=3")
            return 95;
        std::getline(file, line);
        if (line != "# shifter-role=gear-1")
            return 95;
        std::getline(file, line);
        if (line != std::string(nativeShifterPrefix) + deviceId + ",button,0,0,1,0")
            return 95;
    }

    auto edited = profile;
    edited.front().role = ShifterRole::Gear3;
    if (sameAccessoryInputProfile(profile, edited))
        return 96;
    edited = profile;
    edited.front().nativeShifter = false;
    if (sameAccessoryInputProfile(profile, edited))
        return 96;

    const auto rejected = [&](const auto& change) {
        edited = profile;
        change(edited);
        return !saveAccessoryProfile(path, edited, error);
    };
    if (!rejected([](auto& items) { items[0].key = VK_F1; })
        || !rejected([](auto& items) { items[0].activation = AccessoryActivation::pulse; })
        || !rejected([](auto& items) { items[0].role = ShifterRole::Downshift; })
        || !rejected([](auto& items) { items[0].role = items[1].role; })
        || !rejected([](auto& items) { items[0].deviceId = "{44444444-4444-4444-4444-444444444444}"; })
        || !rejected([](auto& items) { items[1].handbrakeCeilingPercent = 15; })
        || !rejected([](auto& items) {
            items[1] = items[0];
            items[1].role = ShifterRole::Gear2;
        })
        || loadAccessoryProfile(path, error) != profile)
        return 97;
    return 0;
}

int testNativeShifterPolling(const std::string& deviceId)
{
    using namespace chrome_axis;
    using Status = DeviceReadStatus;
    AccessoryReader reader({
        {deviceId, AccessoryKind::Button, 0, 0, 1, 0, {}, ShifterRole::Gear1, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Button, 1, 0, 1, 0, {}, ShifterRole::Gear2, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Pov, 0, povCentered, 18000, 0, {}, ShifterRole::Reverse, AccessoryActivation::hold, 0, true},
        {deviceId, AccessoryKind::Button, 2, 0, 1, VK_ADD, {}, ShifterRole::Upshift, AccessoryActivation::pulse},
    });
    DeviceSample sample;
    sample.buttonsPresent.set(0).set(1).set(2);
    sample.povsPresent[0] = true;
    const std::array<const DeviceSample*, 4> connected{&sample, &sample, &sample, &sample};
    const std::array<const DeviceSample*, 4> disconnected{};
    const std::array pending{Status::pending, Status::pending, Status::pending, Status::pending};
    const std::array resumed{Status::resumed, Status::resumed, Status::resumed, Status::resumed};
    std::bitset<256> pulses;
    std::optional<int> gear;
    const auto expect = [&](std::optional<int> expected, int polls = 1) {
        for (int i = 0; i < polls; ++i) {
            if (reader.update(connected, {}, {}, &pulses, nullptr, &gear).any() || gear != expected)
                return false;
        }
        return true;
    };

    sample.buttons.set(0);
    if (!expect(std::nullopt, 5) || reader.ready())
        return 98;
    sample.buttons.reset(0).set(1);
    if (!expect(std::nullopt, 5) || reader.ready())
        return 98;
    sample.buttons.reset(1);
    if (!expect(std::nullopt, 9) || !expect(-1) || !reader.ready())
        return 99;

    sample.buttons.set(0);
    if (!expect(-1, 2) || !expect(1) || pulses.any())
        return 100;
    reader.resetKeyboardBindings();
    if (!expect(1))
        return 100;
    sample.buttons.reset(0);
    if (!expect(1, 2))
        return 101;
    sample.buttons.set(1);
    if (!expect(1, 2) || !expect(2))
        return 101;

    sample.buttons.set(0);
    if (!expect(std::nullopt, 4))
        return 102;
    sample.buttons.reset(0);
    if (!expect(std::nullopt, 2) || !expect(2))
        return 102;
    sample.buttons.reset(1);
    if (!expect(2, 7) || !expect(-1))
        return 103;
    sample.povs[0] = 18000;
    if (!expect(-1, 2) || !expect(0))
        return 104;
    sample.povs[0] = 9000;
    if (!expect(0, 7) || !expect(std::nullopt))
        return 105;
    sample.povs[0] = povCentered;
    if (!expect(std::nullopt, 7) || !expect(-1))
        return 105;

    sample.buttons.set(1);
    if (!expect(-1, 2) || !expect(2))
        return 106;
    sample.buttonsPresent.reset(0);
    sample.buttons.set(2);
    if (!expect(std::nullopt) || !pulses.test(VK_ADD) || pulses.count() != 1)
        return 106;
    sample.buttonsPresent.set(0);
    if (!expect(std::nullopt, 5) || reader.ready())
        return 107;
    if (reader.update(disconnected, pending, {}, &pulses, nullptr, &gear).any() || gear
        || reader.update(connected, resumed, {}, &pulses, nullptr, &gear).any() || gear
        || !expect(std::nullopt, 5))
        return 107;
    sample.buttons.reset();
    if (!expect(std::nullopt, 9) || !expect(-1) || !reader.ready())
        return 108;

    sample.buttons.set(0);
    if (!expect(-1, 2) || !expect(1)
        || reader.update(connected, pending, {}, &pulses, nullptr, &gear).any() || gear)
        return 109;

    reader.reset();
    return expect(std::nullopt, 4) && !reader.ready() ? 0 : 109;
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
    const auto pulseResult = testPulseBindings(path, wheelId);
    const auto handbrakeResult = testProgressiveHandbrake(path, handbrakeId);
    const auto replacementResult = testProfileReplacement(path, wheelId);
    const auto nativeShifterProfileResult = testNativeShifterProfile(path, wheelId);
    std::filesystem::remove(path);
    if (!saved || !rejectedDuplicate || !rejectedZeroTravel || !rejectedDisallowedKey
        || !loaded || *loaded != profile || !savedEmpty || !empty || !empty->empty())
        return 1;
    if (thresholdResult != 0)
        return thresholdResult;
    if (roleResult != 0)
        return roleResult;
    if (pulseResult != 0)
        return pulseResult;
    if (handbrakeResult != 0)
        return handbrakeResult;
    if (replacementResult != 0)
        return replacementResult;
    if (nativeShifterProfileResult != 0)
        return nativeShifterProfileResult;
    if (const auto result = testNativeShifterPolling(wheelId); result != 0)
        return result;
    if (const auto result = testPreviewReadings(wheelId); result != 0)
        return result;

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
