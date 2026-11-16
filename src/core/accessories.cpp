#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"

#include "settings_replace.hpp"

#include <algorithm>
#include <bit>
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
constexpr auto nativeGearDebouncePolls = 3u;
constexpr auto nativeNeutralDebouncePolls = 8u;
constexpr double neutralThreshold = 0.15;
constexpr double handbrakeDeadZone = 0.03;
constexpr double clutchDeadZone = 0.03;
constexpr AccessoryThresholds defaultThresholds{};
constexpr std::array<std::string_view, 3> kindNames{"button", "pov", "axis"};
constexpr std::array<std::string_view, 3> clutchModeNames{"off", "assist", "experiment"};
constexpr std::string_view pulseSuffix = ",pulse";
constexpr std::string_view rolePrefix = "# shifter-role=";
constexpr std::string_view nativeShifterPositionsPrefix = "# native-shifter-positions=";
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

int nativeGearNumber(ShifterRole role) noexcept
{
    const auto position = nativeShifterPositionBit(role);
    return position ? std::countr_zero(position) : -1;
}

bool sameNativeInput(const AccessoryBinding& first, const AccessoryBinding& second) noexcept
{
    if (first.deviceId != second.deviceId || first.kind != second.kind || first.index != second.index)
        return false;
    switch (first.kind) {
    case AccessoryKind::Button:
        return true;
    case AccessoryKind::Pov:
        return first.engaged == second.engaged;
    case AccessoryKind::Axis:
        return (first.engaged > first.rest) == (second.engaged > second.rest);
    }
    return false;
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

bool parseBinding(std::string_view value, AccessoryBinding& binding, int version)
{
    if (version == 3 && value.ends_with(pulseSuffix)) {
        binding.activation = AccessoryActivation::pulse;
        value.remove_suffix(pulseSuffix.size());
    }
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
        if (version < 2 || binding.kind != AccessoryKind::Axis || comma == std::string_view::npos
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

bool parseProgressiveHandbrake(std::string_view value, AccessoryBinding& binding)
{
    const auto capSeparator = value.rfind(',');
    return capSeparator != std::string_view::npos
        && parseBinding(value.substr(0, capSeparator), binding, 1)
        && parseNumber(trim(value.substr(capSeparator + 1)), binding.handbrakeCeilingPercent)
        && validHandbrakeCeilingPercent(binding.handbrakeCeilingPercent);
}

bool parseClutch(std::string_view value, AccessoryBinding& binding)
{
    const auto separator = value.rfind(',');
    if (separator == std::string_view::npos || !parseBinding(value.substr(0, separator), binding, 1))
        return false;
    const auto mode = std::ranges::find(clutchModeNames, trim(value.substr(separator + 1)));
    if (mode == clutchModeNames.end())
        return false;
    binding.clutchMode = static_cast<ClutchMode>(mode - clutchModeNames.begin());
    return true;
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
    std::array<const AccessoryBinding*, 9> nativePositions{};
    std::array<const AccessoryBinding*, 2> paddles{};
    std::string_view nativeDevice;
    bool hasProgressiveHandbrake = false;
    bool hasClutch = false;
    for (const auto& binding : profile) {
        if (!validInput(binding) || static_cast<std::size_t>(binding.role) >= roleNames.size()
            || (binding.activation != AccessoryActivation::hold && binding.activation != AccessoryActivation::pulse)) {
            error = "Invalid accessory input";
            return false;
        }
        if (binding.nativeShifter) {
            const int gear = nativeGearNumber(binding.role);
            if (gear < 0 || binding.key != 0 || binding.activation != AccessoryActivation::hold
                || binding.handbrakeCeilingPercent != 0 || binding.nativeSequential || binding.clutchMode
                || (!nativeDevice.empty() && binding.deviceId != nativeDevice)) {
                error = "Invalid accessory input";
                return false;
            }
            auto& position = nativePositions[static_cast<std::size_t>(gear)];
            if (position || std::ranges::any_of(nativePositions, [&](const AccessoryBinding* other) {
                return other && sameNativeInput(binding, *other);
            })) {
                error = "Invalid accessory input";
                return false;
            }
            position = &binding;
            nativeDevice = binding.deviceId;
            continue;
        }
        if (binding.nativeSequential) {
            const bool up = binding.role == ShifterRole::Upshift;
            const bool down = binding.role == ShifterRole::Downshift;
            if ((!up && !down) || binding.key != 0 || binding.activation != AccessoryActivation::hold
                || binding.handbrakeCeilingPercent || binding.nativeShifterPositions || binding.clutchMode) {
                error = "Invalid native sequential input";
                return false;
            }
            auto& position = paddles[up ? 0 : 1];
            if (position || std::ranges::any_of(paddles, [&](const AccessoryBinding* other) {
                return other && sameNativeInput(binding, *other);
            })) {
                error = "Duplicate native sequential input";
                return false;
            }
            position = &binding;
            continue;
        }
        if (binding.clutchMode) {
            if (hasClutch || binding.kind != AccessoryKind::Axis || binding.key != 0 || binding.role != ShifterRole::None
                || binding.activation != AccessoryActivation::hold || binding.thresholds != defaultThresholds
                || binding.handbrakeCeilingPercent || binding.nativeShifterPositions
                || static_cast<std::size_t>(*binding.clutchMode) >= clutchModeNames.size()) {
                error = "Invalid or duplicate clutch axis";
                return false;
            }
            hasClutch = true;
            continue;
        }
        if (binding.handbrakeCeilingPercent != 0) {
            if (hasProgressiveHandbrake || !validHandbrakeCeilingPercent(binding.handbrakeCeilingPercent)
                || binding.kind != AccessoryKind::Axis || binding.key != 0 || binding.role != ShifterRole::None
                || binding.activation != AccessoryActivation::hold || binding.thresholds != defaultThresholds) {
                error = "Invalid or duplicate progressive handbrake";
                return false;
            }
            hasProgressiveHandbrake = true;
            continue;
        }
        if (!isAccessoryKey(binding.key)) {
            error = "Invalid accessory keyboard key";
            return false;
        }
        if (keys.test(binding.key)) {
            error = "An accessory keyboard key is assigned more than once";
            return false;
        }
        keys.set(binding.key);
    }
    if (!nativeDevice.empty() && (paddles[0] || paddles[1])) {
        error = "Native H-pattern and sequential inputs cannot be active together";
        return false;
    }
    error.clear();
    return true;
}

struct InputLevel {
    bool neutral{};
    bool engaged{};
    float analogValue{};
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
        if (binding.clutchMode) {
            const float command = static_cast<float>(std::clamp((fraction - clutchDeadZone) / (1.0 - clutchDeadZone), 0.0, 1.0));
            return InputLevel{std::abs(fraction) <= clutchDeadZone, command > 0.0f, command};
        }
        if (binding.handbrakeCeilingPercent != 0) {
            const auto adjusted = std::clamp((fraction - handbrakeDeadZone) / (1.0 - handbrakeDeadZone), 0.0, 1.0);
            const auto command = static_cast<float>(adjusted * (binding.handbrakeCeilingPercent / 100.0));
            return InputLevel{std::abs(fraction) <= handbrakeDeadZone, command > 0.0f, command};
        }
        const double releaseThreshold = binding.thresholds.release / 100.0;
        const double pressThreshold = binding.thresholds.press / 100.0;
        // A low release setting also narrows the neutral gate.
        return InputLevel{std::abs(fraction) <= neutralThreshold && fraction < releaseThreshold,
                          fraction >= (active ? releaseThreshold : pressThreshold)};
    }
    }
    return std::nullopt;
}

bool probing(const AccessoryBinding& binding, const DeviceSample& sample) noexcept
{
    switch (binding.kind) {
    case AccessoryKind::Button: return sample.buttonsPending[binding.index];
    case AccessoryKind::Pov: return sample.povsPending[binding.index];
    case AccessoryKind::Axis: return sample.axesPending[binding.index];
    }
    return false;
}

} // namespace

DiscreteAccessoryLevel discreteAccessoryLevel(const AccessoryBinding& binding, const DeviceSample& sample,
                                              bool engaged) noexcept
{
    if (probing(binding, sample))
        return DiscreteAccessoryLevel::pending;
    const auto level = readLevel(binding, sample, engaged, nullptr);
    if (!level)
        return DiscreteAccessoryLevel::unavailable;
    if (level->engaged)
        return DiscreteAccessoryLevel::engaged;
    return level->neutral ? DiscreteAccessoryLevel::neutral : DiscreteAccessoryLevel::idle;
}

namespace {

unsigned int analogAccessoryConflicts(const Profile& axes, const AccessoryBinding& binding) noexcept
{
    if (binding.kind != AccessoryKind::Axis || (!binding.clutchMode && !binding.handbrakeCeilingPercent))
        return 0;

    const auto samePhysicalAxis = [&](const AxisCalibration& axis) {
        return binding.index == axis.axis && binding.deviceId == axis.deviceId;
    };
    // A steering axis is used in both directions. Pedals can share a centered signal
    // only if the two measured travels are opposite and the rest points are mutually neutral.
    const double accessoryTravel = travel(binding);
    const double accessoryDeadzone = binding.clutchMode ? clutchDeadZone : handbrakeDeadZone;
    const auto pedalConflicts = [&](const AxisCalibration& axis) {
        if (!samePhysicalAxis(axis))
            return false;
        const double pedalTravel = static_cast<double>(axis.positive) - axis.neutral;
        const double centerGap = std::abs(static_cast<double>(axis.neutral) - binding.rest);
        return !(pedalTravel * accessoryTravel < 0.0
            && centerGap <= std::abs(pedalTravel) * axis.deadzone
            && centerGap <= std::abs(accessoryTravel) * accessoryDeadzone);
    };

    return (samePhysicalAxis(axes.steering) ? 1u : 0u)
        | (pedalConflicts(axes.throttle) ? 2u : 0u) | (pedalConflicts(axes.brake) ? 4u : 0u);
}

} // namespace

bool analogAccessoryAxisConflict(const Profile& axes, const AccessoryBinding& binding) noexcept
{
    return analogAccessoryConflicts(axes, binding) != 0;
}

bool introducesAnalogAccessoryAxisConflict(const Profile* previousAxes, std::span<const AccessoryBinding> previousBindings,
                                           const Profile& axes, std::span<const AccessoryBinding> bindings) noexcept
{
    for (const auto& binding : bindings) {
        const auto conflicts = analogAccessoryConflicts(axes, binding);
        if (!conflicts)
            continue;
        if (!previousAxes)
            return true;
        const auto previous = std::ranges::find_if(previousBindings, [&](const auto& saved) {
            return saved.kind == binding.kind && saved.deviceId == binding.deviceId &&
                saved.index == binding.index && saved.rest == binding.rest && saved.engaged == binding.engaged &&
                saved.clutchMode.has_value() == binding.clutchMode.has_value() &&
                (saved.handbrakeCeilingPercent != 0) == (binding.handbrakeCeilingPercent != 0);
        });
        if (previous == previousBindings.end() || (conflicts & ~analogAccessoryConflicts(*previousAxes, *previous)))
            return true;
    }
    return false;
}

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
    unsigned int nativePositions{};
    std::string line;
    while (std::getline(file, line)) {
        const auto entry = trim(line);
        if (entry.starts_with(nativeShifterPositionsPrefix)) {
            unsigned int declared{};
            nativePositions = parseNumber(trim(entry.substr(nativeShifterPositionsPrefix.size())), declared)
                ? declared : 0u;
            role = ShifterRole::None;
            continue;
        }
        if (entry.starts_with(progressiveHandbrakePrefix)) {
            AccessoryBinding binding;
            if (!version || profile.size() == maxAccessoryBindings
                || !parseProgressiveHandbrake(entry.substr(progressiveHandbrakePrefix.size()), binding)) {
                error = "Invalid progressive handbrake profile entry";
                return std::nullopt;
            }
            profile.push_back(std::move(binding));
            role = ShifterRole::None;
            nativePositions = 0;
            continue;
        }
        if (entry.starts_with(clutchPrefix)) {
            AccessoryBinding binding;
            if (!version || profile.size() == maxAccessoryBindings
                || !parseClutch(entry.substr(clutchPrefix.size()), binding)) {
                error = "Invalid clutch profile entry";
                return std::nullopt;
            }
            profile.push_back(std::move(binding));
            role = ShifterRole::None;
            nativePositions = 0;
            continue;
        }
        if (entry.starts_with(nativeShifterPrefix) || entry.starts_with(nativeSequentialPrefix)) {
            AccessoryBinding binding;
            binding.nativeShifter = entry.starts_with(nativeShifterPrefix);
            binding.nativeSequential = !binding.nativeShifter;
            const auto prefix = binding.nativeShifter ? nativeShifterPrefix : nativeSequentialPrefix;
            binding.nativeShifterPositions = binding.nativeShifter ? nativePositions : 0u;
            nativePositions = 0;
            binding.role = std::exchange(role, ShifterRole::None);
            if (!version || profile.size() == maxAccessoryBindings
                || !parseBinding(entry.substr(prefix.size()), binding, version)) {
                error = "Invalid accessory profile entry";
                return std::nullopt;
            }
            profile.push_back(std::move(binding));
            continue;
        }
        if (entry.empty() || entry.starts_with('#')) {
            role = parseRole(entry);
            if (role == ShifterRole::None)
                nativePositions = 0;
            continue;
        }
        nativePositions = 0;
        if (!version && (entry == "version=1" || entry == "version=2" || entry == "version=3")) {
            version = entry.back() - '0';
            role = ShifterRole::None;
            continue;
        }
        constexpr std::string_view prefix = "binding=";
        AccessoryBinding binding;
        binding.role = std::exchange(role, ShifterRole::None);
        if (!version || !entry.starts_with(prefix) || profile.size() == maxAccessoryBindings
            || !parseBinding(entry.substr(prefix.size()), binding, version)) {
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

    const auto temporary = detail::stagedSettingsPath(path);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.imbue(std::locale::classic());
    const bool hasCustomThresholds = std::ranges::any_of(profile, [](const AccessoryBinding& binding) {
        return binding.thresholds != defaultThresholds;
    });
    const bool hasPulses = std::ranges::any_of(profile, [](const AccessoryBinding& binding) {
        return binding.activation == AccessoryActivation::pulse;
    });
    const int version = hasPulses ? 3 : (hasCustomThresholds ? 2 : 1);
    file << "# Chrome Axis accessories\nversion=" << version << '\n';
    for (const auto& binding : profile) {
        // Accessory versions 1 and 2 already ignore comment lines.
        if (binding.nativeShifter && binding.nativeShifterPositions)
            file << nativeShifterPositionsPrefix << binding.nativeShifterPositions << '\n';
        if (binding.role != ShifterRole::None)
            file << rolePrefix << roleNames[static_cast<std::size_t>(binding.role)] << '\n';
        // Older versions skip these comment rows and still read the keyboard bindings.
        const bool progressive = binding.handbrakeCeilingPercent != 0;
        const auto prefix = binding.nativeShifter ? nativeShifterPrefix
                          : binding.nativeSequential ? nativeSequentialPrefix : binding.clutchMode ? clutchPrefix
                          : progressive ? progressiveHandbrakePrefix : "binding=";
        file << prefix
             << binding.deviceId << ',' << kindNames[static_cast<std::size_t>(binding.kind)]
             << ',' << binding.index << ',' << binding.rest << ',' << binding.engaged << ',' << binding.key;
        if (progressive)
            file << ',' << binding.handbrakeCeilingPercent;
        if (binding.clutchMode)
            file << ',' << clutchModeNames[static_cast<std::size_t>(*binding.clutchMode)];
        if (binding.thresholds != defaultThresholds)
            file << ',' << binding.thresholds.press << ',' << binding.thresholds.release;
        if (binding.activation == AccessoryActivation::pulse)
            file << pulseSuffix;
        file << '\n';
    }
    file.close();

    if (file && detail::replaceSettingsFile(temporary, path))
        return true;

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "Cannot write the accessory profile. Check the destination and its permissions";
    return false;
}

bool nativeShifterVerified(const AccessoryProfile& profile) noexcept
{
    std::string_view device;
    unsigned int declared{};
    unsigned int present{};
    for (const auto& binding : profile) {
        if (!binding.nativeShifter)
            continue;
        const auto position = nativeShifterPositionBit(binding.role);
        if (!position || (present & position) || !binding.nativeShifterPositions)
            return false;
        if (!present) {
            device = binding.deviceId;
            declared = binding.nativeShifterPositions;
        }
        if (binding.deviceId != device || binding.nativeShifterPositions != declared)
            return false;
        present |= position;
    }
    return declared && declared == present;
}

bool sameAccessoryInputProfile(const std::optional<AccessoryProfile>& first,
                               const std::optional<AccessoryProfile>& second)
{
    if (!first || !second)
        return first.has_value() == second.has_value();
    const auto mapping = [](const AccessoryBinding& binding) {
        return std::tie(binding.deviceId, binding.kind, binding.index, binding.rest, binding.engaged, binding.key,
                        binding.thresholds, binding.activation, binding.handbrakeCeilingPercent,
                        binding.nativeShifter, binding.nativeSequential, binding.clutchMode);
    };
    return std::ranges::equal(*first, *second, [&](const AccessoryBinding& left, const AccessoryBinding& right) {
        return mapping(left) == mapping(right)
            && (!(left.nativeShifter || left.nativeSequential) || left.role == right.role)
            && (!left.nativeShifter || left.nativeShifterPositions == right.nativeShifterPositions);
    });
}

AccessoryReader::AccessoryReader(const AccessoryProfile& profile)
{
    std::string error;
    if (!validateAccessories(profile, error))
        return;
    profile_ = profile;
    nativeShifterVerified_ = nativeShifterVerified(profile_);
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
    selectorSequences_.fill(0);
    nativeShifter_ = {};
    sequentialNeutralPolls_ = 0;
    sequentialWaitForRelease_ = false;
}

void AccessoryReader::resetKeyboardBindings() noexcept
{
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        if (profile_[i].key)
            states_[i] = {};
    }
}

void AccessoryReader::resetDiscrete(std::string_view deviceId) noexcept
{
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto& binding = profile_[i];
        if (binding.deviceId != deviceId || !isDiscreteAccessory(binding))
            continue;
        states_[i] = {};
        if (binding.nativeSequential) {
            selectorSequences_[i] = 0;
            sequentialNeutralPolls_ = 0;
            sequentialWaitForRelease_ = false;
        }
    }
}

void AccessoryReader::resetSequential() noexcept
{
    sequentialNeutralPolls_ = 0;
    sequentialWaitForRelease_ = false;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        if (profile_[i].nativeSequential) {
            states_[i] = {};
            selectorSequences_[i] = 0;
        }
    }
}

std::bitset<256> AccessoryReader::armedKeyboardBindings() const noexcept
{
    std::bitset<256> keys;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        if (profile_[i].key && states_[i].status == AccessoryStatus::armed)
            keys.set(profile_[i].key);
    }
    return keys;
}

bool AccessoryReader::ready() const noexcept
{
    for (const auto& state : std::span(states_).first(profile_.size())) {
        if (state.status != AccessoryStatus::armed)
            return false;
    }
    return !profile_.empty();
}

void removeDisabledDrivingAccessories(AccessoryProfile& profile)
{
    std::erase_if(profile, [](const AccessoryBinding& binding) { return binding.clutchMode == ClutchMode::off; });
}

AccessoryReadiness AccessoryReader::readiness(bool nativeSelectorUsable) const noexcept
{
    AccessoryReadiness result;
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto& state = states_[i];
        result.armed += state.status == AccessoryStatus::armed && (nativeSelectorUsable || !profile_[i].nativeShifter);
        result.deviceUnavailable |= state.status == AccessoryStatus::deviceUnavailable;
        result.inputUnavailable |= state.status == AccessoryStatus::inputUnavailable;
        result.temporarilyUnavailable |= state.status == AccessoryStatus::temporarilyUnavailable;
    }
    return result;
}

std::bitset<256> AccessoryReader::poll(DeviceSampleSource& input, std::span<AccessoryObservation> observations,
                                      std::bitset<256>* pulses, std::optional<float>* progressiveHandbrake,
                                      std::optional<int>* nativeGear, AccessoryTransmissionSample* transmission,
                                      AccessoryKeyTimes* keyTimes, bool replayDiscrete)
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
    return update(std::span(samples).first(profile_.size()), std::span(statuses).first(profile_.size()),
                  observations, pulses, progressiveHandbrake, nativeGear, transmission, keyTimes, replayDiscrete);
}

float AccessoryReader::advance(const AccessoryBinding& binding, BindingState& state, const DeviceSample* sample,
                              DeviceReadStatus status, AccessoryObservation* observation, bool* neutral)
{
    if (status == DeviceReadStatus::resumed)
        state = {};
    const bool fresh = sample && (!sample->sequence || state.sequence != sample->sequence);
    if (sample)
        state.sequence = sample->sequence;
    const bool probePending = sample && probing(binding, *sample);
    if ((!sample && status == DeviceReadStatus::pending) || probePending) {
        if (state.neutralPolls < requiredNeutralPolls)
            state.neutralPolls = 0;
        else
            state.waitForRelease = true;
        state.active = false;
        state.status = AccessoryStatus::temporarilyUnavailable;
        return 0.0f;
    }
    const auto level = sample ? readLevel(binding, *sample, state.active, observation ? &observation->travel : nullptr)
                              : std::nullopt;
    if (!level) {
        state = {};
        state.status = sample ? AccessoryStatus::inputUnavailable : AccessoryStatus::deviceUnavailable;
        return 0.0f;
    }
    if (observation) {
        observation->engaged = level->engaged;
        observation->neutral = level->neutral;
        observation->sampledAt = sample->sampledAt;
    }
    if (neutral)
        *neutral = level->neutral;
    if (state.neutralPolls < requiredNeutralPolls) {
        if (!level->neutral)
            state.neutralPolls = 0;
        else if (fresh)
            ++state.neutralPolls;
        state.status = state.neutralPolls == requiredNeutralPolls
            ? AccessoryStatus::armed : AccessoryStatus::waitingForNeutral;
        return 0.0f;
    }
    if (state.waitForRelease) {
        state.waitForRelease = !level->neutral;
        state.status = state.waitForRelease ? AccessoryStatus::waitingForNeutral : AccessoryStatus::armed;
        return 0.0f;
    }
    state.status = AccessoryStatus::armed;
    state.active = level->engaged;
    return level->analogValue;
}

std::bitset<256> AccessoryReader::update(std::span<const DeviceSample* const> samples,
                                       std::span<const DeviceReadStatus> statuses,
                                       std::span<AccessoryObservation> observations, std::bitset<256>* pulses,
                                        std::optional<float>* progressiveHandbrake, std::optional<int>* nativeGear,
                                        AccessoryTransmissionSample* transmission, AccessoryKeyTimes* keyTimes,
                                        bool replayDiscrete)
{
    std::bitset<256> keys;
    std::array<std::uint64_t, maxAccessoryBindings> sampleSequences{};
    bool nativeFresh = !replayDiscrete, paddlesFresh = true;
    DeviceReadClock::time_point gearSampledAt{}, paddlesSampledAt{}, shiftSampledAt{};
    std::array<DeviceReadClock::time_point, 2> paddleSampleTimes{};
    const auto oldestSample = [](auto& oldest, const DeviceSample* sample) {
        if (sample && sample->sampledAt != DeviceReadClock::time_point{} &&
            (oldest == DeviceReadClock::time_point{} || sample->sampledAt < oldest))
            oldest = sample->sampledAt;
    };
    bool hasNativePositions = false;
    bool nativeAvailable = nativeShifterVerified_;
    bool nativeArmed = true;
    bool allNativeNeutral = true;
    unsigned int activePositions{};
    int selectedGear{};
    unsigned int paddlePositions{};
    unsigned int pressedPaddles{};
    bool paddlesAvailable = true;
    bool paddlesArmed = true;
    bool paddlesNeutral = true;
    int paddleEdge{};
    if (pulses)
        pulses->reset();
    if (progressiveHandbrake)
        progressiveHandbrake->reset();
    if (nativeGear)
        nativeGear->reset();
    if (transmission)
        *transmission = {};
    if (keyTimes)
        keyTimes->fill({});
    for (std::size_t i = 0; i < profile_.size(); ++i) {
        const auto& binding = profile_[i];
        if (replayDiscrete && !isDiscreteAccessory(binding))
            continue;
        auto& state = states_[i];
        const auto* sample = i < samples.size() ? samples[i] : nullptr;
        const auto status = i < statuses.size() ? statuses[i] : DeviceReadStatus::unavailable;
        if ((binding.nativeShifter || binding.nativeSequential || binding.clutchMode) && i < statuses.size() &&
            (status == DeviceReadStatus::pending || status == DeviceReadStatus::unavailable))
            sample = nullptr;
        auto* observation = i < observations.size() ? &observations[i] : nullptr;
        if (observation)
            *observation = {};
        const bool wasActive = state.active;
        bool neutral = false;
        const auto analogValue = advance(binding, state, sample, status, observation,
                                         binding.nativeShifter || binding.nativeSequential ? &neutral : nullptr);
        if (observation) {
            observation->status = state.status;
            observation->active = state.active;
            observation->key = binding.key;
        }
        if (binding.nativeShifter) {
            hasNativePositions = true;
            sampleSequences[i] = sample ? sample->sequence : 0;
            nativeFresh &= sample && (!sample->sequence || sample->sequence != selectorSequences_[i]);
            oldestSample(gearSampledAt, sample);
            nativeAvailable &= status != DeviceReadStatus::resumed &&
                (state.status == AccessoryStatus::armed || state.status == AccessoryStatus::waitingForNeutral);
            nativeArmed &= state.status == AccessoryStatus::armed;
            allNativeNeutral &= neutral;
            if (state.active) {
                ++activePositions;
                selectedGear = nativeGearNumber(binding.role);
            }
            continue;
        }
        if (binding.nativeSequential) {
            sampleSequences[i] = sample ? sample->sequence : 0;
            paddlesFresh &= sample && (!sample->sequence || sample->sequence != selectorSequences_[i]);
            oldestSample(paddlesSampledAt, sample);
            const bool up = binding.role == ShifterRole::Upshift;
            if (sample)
                paddleSampleTimes[up ? 0 : 1] = sample->sampledAt;
            paddlePositions |= up ? 1u : 2u;
            const bool armed = state.status == AccessoryStatus::armed;
            paddlesAvailable &= status != DeviceReadStatus::resumed &&
                (armed || state.status == AccessoryStatus::waitingForNeutral);
            paddlesArmed &= armed;
            paddlesNeutral &= neutral;
            if (state.active) {
                ++pressedPaddles;
                if (!wasActive) {
                    paddleEdge = up ? 1 : -1;
                    shiftSampledAt = sample->sampledAt;
                }
            }
            continue;
        }
        if (binding.clutchMode) {
            if (transmission) {
                if (observation)
                    transmission->clutchObservation = *observation;
                if (state.status == AccessoryStatus::armed) {
                    transmission->clutch = analogValue;
                    transmission->clutchSampledAt = sample->sampledAt;
                }
            }
            continue;
        }
        if (binding.handbrakeCeilingPercent != 0) {
            if (progressiveHandbrake && state.status == AccessoryStatus::armed) {
                *progressiveHandbrake = analogValue;
                if (transmission)
                    transmission->handbrakeSampledAt = sample->sampledAt;
            }
            continue;
        }
        if (!state.active)
            continue;
        if (binding.activation == AccessoryActivation::hold)
            keys.set(binding.key);
        else if (!wasActive && pulses)
            pulses->set(binding.key);
        else
            continue;
        if (keyTimes)
            (*keyTimes)[binding.key] = std::max((*keyTimes)[binding.key], sample->sampledAt);
    }

    // Each selector uses new reports across all its positions or paddles.
    if (nativeFresh || paddlesFresh) {
        for (std::size_t i = 0; i < profile_.size(); ++i) {
            if ((nativeFresh && profile_[i].nativeShifter) || (paddlesFresh && profile_[i].nativeSequential))
                selectorSequences_[i] = sampleSequences[i];
        }
    }
    if (paddlePositions) {
        if (paddlePositions != 3 || !paddlesAvailable) {
            sequentialNeutralPolls_ = 0;
            sequentialWaitForRelease_ = false;
        } else if (sequentialNeutralPolls_ < requiredNeutralPolls) {
            if (!paddlesNeutral)
                sequentialNeutralPolls_ = 0;
            else if (paddlesFresh)
                ++sequentialNeutralPolls_;
        }
        const bool armed = paddlesArmed && sequentialNeutralPolls_ == requiredNeutralPolls;
        if (armed) {
            // An overlapping press must be followed by both paddles returning to rest.
            sequentialWaitForRelease_ = pressedPaddles > 1 || (sequentialWaitForRelease_ && !paddlesNeutral);
            if (transmission) {
                transmission->sequentialArmed = true;
                transmission->paddlesPressed = pressedPaddles;
                transmission->sequentialSampledAt = paddlesSampledAt;
                if (!sequentialWaitForRelease_ && pressedPaddles == 1) {
                    transmission->shiftDelta = paddleEdge;
                    transmission->shiftSampledAt = shiftSampledAt;
                    if (paddleEdge)
                        transmission->oppositePaddleSampledAt = paddleSampleTimes[paddleEdge > 0 ? 1 : 0];
                }
            }
        } else {
            for (std::size_t i = 0; i < profile_.size(); ++i) {
                if (!profile_[i].nativeSequential || states_[i].status != AccessoryStatus::armed)
                    continue;
                states_[i].status = AccessoryStatus::waitingForNeutral;
                if (i < observations.size()) {
                    observations[i].status = AccessoryStatus::waitingForNeutral;
                    observations[i].active = false;
                }
            }
        }
    }

    if (!hasNativePositions)
        return keys;

    if (transmission)
        transmission->selectorAmbiguous = activePositions > 1;

    if (!nativeAvailable) {
        nativeShifter_ = {};
    } else if (nativeShifter_.neutralPolls < requiredNeutralPolls) {
        // All configured positions must pass through true neutral together after a loss.
        if (!allNativeNeutral)
            nativeShifter_.neutralPolls = 0;
        else if (nativeFresh)
            ++nativeShifter_.neutralPolls;
    }

    if (nativeShifter_.neutralPolls < requiredNeutralPolls) {
        for (std::size_t i = 0; i < profile_.size(); ++i) {
            if (!profile_[i].nativeShifter || states_[i].status != AccessoryStatus::armed)
                continue;
            states_[i].status = AccessoryStatus::waitingForNeutral;
            if (i < observations.size()) {
                observations[i].status = AccessoryStatus::waitingForNeutral;
                observations[i].active = false;
            }
        }
        return keys;
    }

    if (!nativeArmed || activePositions > 1) {
        nativeShifter_.candidate.reset();
        nativeShifter_.stable.reset();
        nativeShifter_.candidatePolls = 0;
        return keys;
    }

    // A brief gap or partial signal keeps the last stable gear. Only true rest can become neutral.
    const std::optional<int> candidate = activePositions == 1 ? std::optional{selectedGear}
        : allNativeNeutral ? std::optional{-1} : std::nullopt;
    const auto required = candidate && *candidate != -1 ? nativeGearDebouncePolls : nativeNeutralDebouncePolls;
    if (nativeShifter_.candidate != candidate) {
        nativeShifter_.candidate = candidate;
        nativeShifter_.candidatePolls = 1;
    } else if (nativeFresh && nativeShifter_.candidatePolls < required) {
        ++nativeShifter_.candidatePolls;
    }
    if (nativeShifter_.candidatePolls == required)
        nativeShifter_.stable = candidate;
    if (nativeGear)
        *nativeGear = nativeShifter_.stable;
    if (transmission && nativeShifter_.stable)
        transmission->gearSampledAt = gearSampledAt;
    return keys;
}

} // namespace chrome_axis
