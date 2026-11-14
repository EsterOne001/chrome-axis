#include "chrome_axis/accessories.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <string_view>
#include <tuple>
#include <utility>

namespace chrome_axis {
namespace {

constexpr auto requiredNeutralPolls = 3u;
constexpr double neutralThreshold = 0.15;
constexpr AccessoryThresholds defaultThresholds{};
constexpr std::array<std::string_view, 3> kindNames{"button", "pov", "axis"};
constexpr std::string_view rolePrefix = "# shifter-role=";
constexpr std::array<std::string_view, 12> roleNames{
    "", "gear-1", "gear-2", "gear-3", "gear-4", "gear-5", "gear-6", "gear-7", "gear-8",
    "reverse", "upshift", "downshift"
};
static_assert(roleNames.size() == static_cast<std::size_t>(ShifterRole::Downshift) + 1);

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

ShifterRole parseRole(std::string_view entry)
{
    if (!entry.starts_with(rolePrefix))
        return ShifterRole::None;
    const auto role = std::ranges::find(roleNames, trim(entry.substr(rolePrefix.size())));
    return role == roleNames.end() ? ShifterRole::None : static_cast<ShifterRole>(role - roleNames.begin());
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

bool parseBinding(std::string_view value, AccessoryBinding& binding, bool allowsThresholds)
{
    std::array<std::string_view, 6> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto comma = value.find(',');
        if (i + 1 != fields.size() && comma == std::string_view::npos)
            return false;
        fields[i] = trim(value.substr(0, comma));
        if (comma == std::string_view::npos) {
            value = {};
        } else {
            value.remove_prefix(comma + 1);
            if (value.empty())
                return false;
        }
    }
    std::size_t kind = 0;
    while (kind < kindNames.size() && kindNames[kind] != fields[1])
        ++kind;
    if (kind == kindNames.size())
        return false;

    binding.deviceId = fields[0];
    binding.kind = static_cast<AccessoryKind>(kind);
    if (!value.empty()) {
        const auto comma = value.find(',');
        if (!allowsThresholds || binding.kind != AccessoryKind::Axis || comma == std::string_view::npos
            || !parseNumber(trim(value.substr(0, comma)), binding.thresholds.press)
            || !parseNumber(trim(value.substr(comma + 1)), binding.thresholds.release)) {
            return false;
        }
    }
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
    if (binding.kind != AccessoryKind::Axis && binding.thresholds != defaultThresholds)
        return false;
    switch (binding.kind) {
    case AccessoryKind::Button:
        return binding.index < buttonCount && binding.rest == 0 && binding.engaged == 1;
    case AccessoryKind::Pov:
        return binding.index < povCount && binding.rest == povCentered
            && binding.engaged >= 0 && binding.engaged < povFullTurn && binding.engaged % povDirectionStep == 0;
    case AccessoryKind::Axis:
        return binding.index < axisCount && std::abs(travel(binding)) >= 16.0
            && validAccessoryThresholds(binding.thresholds);
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
        if (!validInput(binding) || !isAccessoryKey(binding.key)
            || static_cast<std::size_t>(binding.role) >= roleNames.size()) {
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

std::optional<InputLevel> readLevel(const AccessoryBinding& binding, const DeviceSample& sample, bool active,
                                   std::optional<double>* travelReading)
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
        const auto angle = quantizePov(sample.povs[binding.index]);
        if (angle == povCentered)
            return InputLevel{true, false};
        return InputLevel{false, angle == binding.engaged};
    }
    case AccessoryKind::Axis: {
        if (!sample.axesPresent[binding.index])
            return std::nullopt;
        const double fraction = (static_cast<double>(sample.axes[binding.index]) - binding.rest) / travel(binding);
        if (travelReading)
            *travelReading = fraction;
        const double releaseThreshold = binding.thresholds.release / 100.0;
        const double pressThreshold = binding.thresholds.press / 100.0;
        // A low release setting also narrows the neutral gate.
        return InputLevel{std::abs(fraction) <= neutralThreshold && fraction < releaseThreshold,
                          fraction >= (active ? releaseThreshold : pressThreshold)};
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
    int version = 0;
    auto role = ShifterRole::None;
    std::string line;
    while (std::getline(file, line)) {
        const auto entry = trim(line);
        if (entry.empty() || entry.starts_with('#')) {
            role = parseRole(entry);
            continue;
        }
        if (!version && (entry == "version=1" || entry == "version=2")) {
            version = entry.back() - '0';
            role = ShifterRole::None;
            continue;
        }
        constexpr std::string_view prefix = "binding=";
        AccessoryBinding binding;
        binding.role = std::exchange(role, ShifterRole::None);
        if (!version || !entry.starts_with(prefix) || profile.size() == maxAccessoryBindings
            || !parseBinding(entry.substr(prefix.size()), binding, version == 2)) {
            error = "Invalid accessory profile entry";
            return std::nullopt;
        }
        profile.push_back(std::move(binding));
    }
    if (file.bad() || !version) {
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
    const bool hasCustomThresholds = std::ranges::any_of(profile, [](const AccessoryBinding& binding) {
        return binding.thresholds != defaultThresholds;
    });
    file << "# Chrome Axis accessories\nversion=" << (hasCustomThresholds ? 2 : 1) << '\n';
    for (const auto& binding : profile) {
        // Accessory versions 1 and 2 already ignore comment lines.
        if (binding.role != ShifterRole::None)
            file << rolePrefix << roleNames[static_cast<std::size_t>(binding.role)] << '\n';
        file << "binding=" << binding.deviceId << ',' << kindNames[static_cast<std::size_t>(binding.kind)]
             << ',' << binding.index << ',' << binding.rest << ',' << binding.engaged << ',' << binding.key;
        if (binding.thresholds != defaultThresholds)
            file << ',' << binding.thresholds.press << ',' << binding.thresholds.release;
        file << '\n';
    }
    file.close();

    if (file && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "Cannot write the accessory profile. Check the destination and its permissions";
    return false;
}

bool sameAccessoryInputProfile(const std::optional<AccessoryProfile>& first,
                               const std::optional<AccessoryProfile>& second)
{
    if (!first || !second)
        return first.has_value() == second.has_value();
    const auto mapping = [](const AccessoryBinding& binding) {
        return std::tie(binding.deviceId, binding.kind, binding.index, binding.rest, binding.engaged, binding.key,
                        binding.thresholds);
    };
    return std::ranges::equal(*first, *second, std::ranges::equal_to{}, mapping, mapping);
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
        if (state.status != AccessoryStatus::armed)
            return false;
    }
    return !profile_.empty();
}

AccessoryReadiness AccessoryReader::readiness() const noexcept
{
    AccessoryReadiness result;
    for (const auto& state : std::span(states_).first(profile_.size())) {
        result.armed += state.status == AccessoryStatus::armed;
        result.deviceUnavailable |= state.status == AccessoryStatus::deviceUnavailable;
        result.inputUnavailable |= state.status == AccessoryStatus::inputUnavailable;
        result.temporarilyUnavailable |= state.status == AccessoryStatus::temporarilyUnavailable;
    }
    return result;
}

std::bitset<256> AccessoryReader::poll(DirectInput& input, std::span<AccessoryObservation> observations)
{
    std::array<std::optional<DeviceSample>, maxAccessoryBindings> storage;
    std::array<const DeviceSample*, maxAccessoryBindings> samples{};
    std::array<DeviceReadStatus, maxAccessoryBindings> statuses{};
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto owner = sampleOwners_[i];
        if (owner == i)
            storage[owner] = input.sample(profile_[i].deviceId, requiredButtons_[owner], &statuses[owner]);
        else
            statuses[i] = statuses[owner];
        if (storage[owner])
            samples[i] = &*storage[owner];
    }
    return update(std::span(samples).first(profile_.size()), std::span(statuses).first(profile_.size()), observations);
}

void AccessoryReader::advance(const AccessoryBinding& binding, BindingState& state, const DeviceSample* sample,
                              DeviceReadStatus status, std::optional<double>* travelReading)
{
    if (!sample && status == DeviceReadStatus::pending) {
        if (state.neutralPolls < requiredNeutralPolls)
            state.neutralPolls = 0;
        else
            state.waitForRelease = true;
        state.active = false;
        state.status = AccessoryStatus::temporarilyUnavailable;
        return;
    }
    if (status == DeviceReadStatus::resumed)
        state = {};
    const auto level = sample ? readLevel(binding, *sample, state.active, travelReading) : std::nullopt;
    if (!level) {
        state = {};
        state.status = sample ? AccessoryStatus::inputUnavailable : AccessoryStatus::deviceUnavailable;
        return;
    }
    if (state.neutralPolls < requiredNeutralPolls) {
        state.neutralPolls = level->neutral ? state.neutralPolls + 1 : 0;
        state.status = state.neutralPolls == requiredNeutralPolls
            ? AccessoryStatus::armed : AccessoryStatus::waitingForNeutral;
        return;
    }
    if (state.waitForRelease) {
        state.waitForRelease = !level->neutral;
        state.status = state.waitForRelease ? AccessoryStatus::waitingForNeutral : AccessoryStatus::armed;
        return;
    }
    state.status = AccessoryStatus::armed;
    state.active = level->engaged;
}

std::bitset<256> AccessoryReader::update(std::span<const DeviceSample* const> samples,
                                       std::span<const DeviceReadStatus> statuses,
                                       std::span<AccessoryObservation> observations)
{
    std::bitset<256> keys;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto& binding = profile_[i];
        auto& state = states_[i];
        const auto* sample = i < samples.size() ? samples[i] : nullptr;
        const auto status = i < statuses.size() ? statuses[i] : DeviceReadStatus::unavailable;
        auto* observation = i < observations.size() ? &observations[i] : nullptr;
        if (observation)
            observation->travel.reset();
        advance(binding, state, sample, status, observation ? &observation->travel : nullptr);
        if (observation) {
            observation->status = state.status;
            observation->active = state.active;
        }
        if (state.active)
            keys.set(binding.key);
    }
    return keys;
}

} // namespace chrome_axis
