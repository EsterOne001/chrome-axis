#include "chrome_axis/accessories.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <string_view>
#include <utility>

namespace chrome_axis {
namespace {

constexpr auto requiredNeutralPolls = 3u;
constexpr double neutralThreshold = 0.15;
constexpr double engageThreshold = 0.55;
constexpr double releaseThreshold = 0.35;
constexpr int povStep = 4500;
constexpr int povFullTurn = 36000;
constexpr std::array<std::string_view, 3> kindNames{"button", "pov", "axis"};

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

template <class T>
bool parseNumber(std::string_view value, T& result)
{
    if (value.empty())
        return false;
    const auto* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseBinding(std::string_view value, AccessoryBinding& binding)
{
    std::array<std::string_view, 6> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto comma = value.find(',');
        const bool last = i + 1 == fields.size();
        if (last != (comma == std::string_view::npos))
            return false;
        fields[i] = trim(value.substr(0, comma));
        if (!last)
            value.remove_prefix(comma + 1);
    }
    std::size_t kind = 0;
    while (kind < kindNames.size() && kindNames[kind] != fields[1])
        ++kind;
    if (kind == kindNames.size())
        return false;

    binding.deviceId = fields[0];
    binding.kind = static_cast<AccessoryKind>(kind);
    return parseNumber(fields[2], binding.index)
        && parseNumber(fields[3], binding.rest)
        && parseNumber(fields[4], binding.engaged)
        && parseNumber(fields[5], binding.key);
}

double travel(const AccessoryBinding& binding)
{
    return static_cast<double>(binding.engaged) - binding.rest;
}

bool validInput(const AccessoryBinding& binding)
{
    if (!isDeviceId(binding.deviceId))
        return false;
    switch (binding.kind) {
    case AccessoryKind::Button:
        return binding.index < buttonCount && binding.rest == 0 && binding.engaged == 1;
    case AccessoryKind::Pov:
        return binding.index < povCount && binding.rest == povCentered
            && binding.engaged >= 0 && binding.engaged < povFullTurn && binding.engaged % povStep == 0;
    case AccessoryKind::Axis:
        return binding.index < axisCount && std::abs(travel(binding)) >= 16.0;
    }
    return false;
}

bool validateAccessories(const AccessoryProfile& profile, std::string& error)
{
    if (profile.size() > maxAccessoryBindings) {
        error = "Too many accessory bindings";
        return false;
    }
    std::bitset<256> keys;
    for (const auto& binding : profile) {
        if (!validInput(binding) || !isAccessoryKey(binding.key)) {
            error = "Invalid accessory input or keyboard key";
            return false;
        }
        if (keys.test(binding.key)) {
            error = "An accessory keyboard key is assigned more than once";
            return false;
        }
        keys.set(binding.key);
    }
    error.clear();
    return true;
}

struct InputLevel {
    bool neutral{};
    bool engaged{};
};

std::optional<InputLevel> readLevel(const AccessoryBinding& binding, const DeviceSample& sample, bool active)
{
    switch (binding.kind) {
    case AccessoryKind::Button: {
        if (!sample.buttonsPresent.test(binding.index))
            return std::nullopt;
        const bool pressed = sample.buttons.test(binding.index);
        return InputLevel{!pressed, pressed};
    }
    case AccessoryKind::Pov: {
        if (!sample.povsPresent[binding.index])
            return std::nullopt;
        const auto angle = sample.povs[binding.index];
        if (angle == povCentered)
            return InputLevel{true, false};
        const int nearest = ((angle + povStep / 2) / povStep * povStep) % povFullTurn;
        return InputLevel{false, nearest == binding.engaged};
    }
    case AccessoryKind::Axis: {
        if (!sample.axesPresent[binding.index])
            return std::nullopt;
        const double fraction = (static_cast<double>(sample.axes[binding.index]) - binding.rest) / travel(binding);
        return InputLevel{std::abs(fraction) <= neutralThreshold,
                          fraction >= (active ? releaseThreshold : engageThreshold)};
    }
    }
    return std::nullopt;
}

} // namespace

bool isAccessoryKey(unsigned int key) noexcept
{
    return key == VK_SPACE || (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')
        || (key >= VK_F1 && key <= VK_F12) || (key >= VK_LEFT && key <= VK_DOWN)
        || (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) || key == VK_ADD || key == VK_SUBTRACT;
}

std::optional<AccessoryProfile> loadAccessoryProfile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Accessory profile is missing or unreadable";
        return std::nullopt;
    }
    const auto size = file.tellg();
    if (size < 0 || size > 16 * 1024) {
        error = "Invalid accessory profile size";
        return std::nullopt;
    }
    file.seekg(0);

    AccessoryProfile profile;
    bool versionRead = false;
    std::string line;
    while (std::getline(file, line)) {
        const auto entry = trim(line);
        if (entry.empty() || entry.starts_with('#'))
            continue;
        if (!versionRead && entry == "version=1") {
            versionRead = true;
            continue;
        }
        constexpr std::string_view prefix = "binding=";
        AccessoryBinding binding;
        if (!versionRead || !entry.starts_with(prefix) || profile.size() == maxAccessoryBindings
            || !parseBinding(entry.substr(prefix.size()), binding)) {
            error = "Invalid accessory profile entry";
            return std::nullopt;
        }
        profile.push_back(std::move(binding));
    }
    if (file.bad() || !versionRead) {
        error = "Unsupported or unreadable accessory profile version";
        return std::nullopt;
    }
    if (!validateAccessories(profile, error))
        return std::nullopt;
    return profile;
}

bool saveAccessoryProfile(const std::filesystem::path& path, const AccessoryProfile& profile, std::string& error)
{
    if (!validateAccessories(profile, error))
        return false;

    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.imbue(std::locale::classic());
    file << "# Chrome Axis accessories\nversion=1\n";
    for (const auto& binding : profile) {
        file << "binding=" << binding.deviceId << ',' << kindNames[static_cast<std::size_t>(binding.kind)]
             << ',' << binding.index << ',' << binding.rest << ',' << binding.engaged << ',' << binding.key << '\n';
    }
    file.close();

    if (file && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "Cannot write the accessory profile. Check the destination and its permissions";
    return false;
}

AccessoryReader::AccessoryReader(const AccessoryProfile& profile)
{
    std::string error;
    if (!validateAccessories(profile, error))
        return;
    profile_ = profile;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        std::size_t owner = 0;
        while (owner < i && profile_[owner].deviceId != profile_[i].deviceId)
            ++owner;
        sampleOwners_[i] = owner;
        if (profile_[i].kind == AccessoryKind::Button)
            requiredButtons_[owner].set(profile_[i].index);
    }
}

void AccessoryReader::reset() noexcept
{
    states_.fill({});
}

bool AccessoryReader::ready() const noexcept
{
    for (const auto& state : std::span(states_).first(profile_.size())) {
        if (state.neutralPolls < requiredNeutralPolls)
            return false;
    }
    return !profile_.empty();
}

std::bitset<256> AccessoryReader::poll(DirectInput& input)
{
    std::array<std::optional<DeviceSample>, maxAccessoryBindings> storage;
    std::array<const DeviceSample*, maxAccessoryBindings> samples{};
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto owner = sampleOwners_[i];
        if (owner == i)
            storage[owner] = input.sample(profile_[i].deviceId, requiredButtons_[owner]);
        if (storage[owner])
            samples[i] = &*storage[owner];
    }
    return update({samples.data(), profile_.size()});
}

std::bitset<256> AccessoryReader::update(std::span<const DeviceSample* const> samples)
{
    std::bitset<256> keys;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto& binding = profile_[i];
        auto& state = states_[i];
        const auto* sample = i < samples.size() ? samples[i] : nullptr;
        const auto level = sample ? readLevel(binding, *sample, state.active) : std::nullopt;
        if (!level) {
            state = {};
            continue;
        }
        if (state.neutralPolls < requiredNeutralPolls) {
            state.neutralPolls = level->neutral ? state.neutralPolls + 1 : 0;
            continue;
        }
        state.active = level->engaged;
        if (state.active)
            keys.set(binding.key);
    }
    return keys;
}

} // namespace chrome_axis
