#include "chrome_axis/controls.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr std::array<std::string_view, 3> channelNames{"steering", "throttle", "brake"};
constexpr unsigned int requiredNeutralPolls = 3;

std::array<const AxisCalibration*, 3> channels(const Profile& profile)
{
    return {&profile.steering, &profile.throttle, &profile.brake};
}

double distance(long value, long origin)
{
    return static_cast<double>(value) - origin;
}

bool sameAxis(const AxisCalibration& first, const AxisCalibration& second) noexcept
{
    return !first.deviceId.empty() && first.axis == second.axis && first.deviceId == second.deviceId;
}

float normalize(const AxisCalibration& calibration, long raw, bool steering)
{
    const double delta = distance(raw, calibration.neutral);
    const double positive = distance(calibration.positive, calibration.neutral);
    const bool positiveSide = delta / positive >= 0.0;
    const double span = positiveSide || !steering
        ? positive
        : distance(calibration.negative, calibration.neutral);
    const auto magnitude = static_cast<float>(std::clamp(delta / span, 0.0, 1.0));
    const float value = std::max(0.0f, magnitude - calibration.deadzone) / (1.0f - calibration.deadzone);
    return steering && !positiveSide ? -value : value;
}

bool isNeutral(const AxisCalibration& calibration, long raw, bool steering, bool combinedPedals)
{
    const double delta = distance(raw, calibration.neutral);
    const double positive = distance(calibration.positive, calibration.neutral);
    const double span = steering && delta / positive < 0.0
        ? distance(calibration.negative, calibration.neutral)
        : positive;
    const double progress = delta / span;
    // A dedicated pedal can return beyond its measured rest position and still produce zero.
    // Shared pedals must stay near the common center so neither is held during arming.
    return (steering || combinedPedals ? std::abs(progress) : progress) <= calibration.deadzone;
}

std::string_view trim(std::string_view value)
{
    constexpr std::string_view space = " \t\r\n";
    const auto first = value.find_first_not_of(space);
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(space) - first + 1);
}

template<class T>
bool parseNumber(const std::string& value, T& result)
{
    const char* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

} // namespace

bool compatibleAxisPair(const AxisCalibration& throttle, const AxisCalibration& brake) noexcept
{
    if (!sameAxis(throttle, brake))
        return true;

    const double throttleTravel = distance(throttle.positive, throttle.neutral);
    const double brakeTravel = distance(brake.positive, brake.neutral);
    const double centerGap = std::abs(distance(throttle.neutral, brake.neutral));
    return throttleTravel * brakeTravel < 0.0
        && centerGap <= std::abs(throttleTravel) * throttle.deadzone
        && centerGap <= std::abs(brakeTravel) * brake.deadzone;
}

bool validateProfile(const Profile& profile, std::string& error)
{
    const auto axes = channels(profile);
    bool anyAssigned = false;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& axis = *axes[i];
        if (axis.deviceId.empty())
            continue;
        anyAssigned = true;
        const double positive = distance(axis.positive, axis.neutral);
        const double negative = distance(axis.negative, axis.neutral);
        const bool validRange = std::abs(positive) >= 16.0
            && (i != 0 || (std::abs(negative) >= 16.0 && positive * negative < 0.0));
        if (!isDeviceId(axis.deviceId) || axis.axis >= axisCount || !validRange
            || !std::isfinite(axis.deadzone) || axis.deadzone < 0.005f || axis.deadzone > 0.25f) {
            error = "Invalid calibration for " + std::string(channelNames[i]);
            return false;
        }
    }
    if (!anyAssigned) {
        error = "At least one control must be calibrated";
        return false;
    }
    if (sameAxis(profile.steering, profile.throttle) || sameAxis(profile.steering, profile.brake)) {
        error = "Steering requires its own physical axis";
        return false;
    }
    if (!compatibleAxisPair(profile.throttle, profile.brake)) {
        error = "Pedals on a shared axis must travel in opposite directions from the same rest position";
        return false;
    }
    error.clear();
    return true;
}

std::optional<Profile> loadProfile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Profile is missing or unreadable. Run the Chrome Axis calibration app (ChromeAxis.exe).";
        return std::nullopt;
    }
    const auto size = file.tellg();
    if (size < 0 || size > 16 * 1024) {
        error = "Invalid profile size";
        return std::nullopt;
    }
    file.seekg(0);

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const auto entry = trim(line);
        if (entry.empty() || entry.starts_with('#'))
            continue;
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos
            || !values.emplace(trim(entry.substr(0, equals)), trim(entry.substr(equals + 1))).second) {
            error = "Invalid or duplicate profile entry";
            return std::nullopt;
        }
    }
    if (file.bad() || values["version"] != "1") {
        error = "Unsupported or unreadable profile version";
        return std::nullopt;
    }

    Profile profile;
    const std::array axes{&profile.steering, &profile.throttle, &profile.brake};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const std::string prefix = std::string(channelNames[i]) + '.';
        const auto first = values.lower_bound(prefix);
        if (first == values.end() || !first->first.starts_with(prefix))
            continue;

        auto& axis = *axes[i];
        axis.deviceId = values[prefix + "device"];
        if (axis.deviceId.empty() || !parseNumber(values[prefix + "axis"], axis.axis)
            || !parseNumber(values[prefix + "neutral"], axis.neutral)
            || !parseNumber(values[prefix + "negative"], axis.negative)
            || !parseNumber(values[prefix + "positive"], axis.positive)
            || !parseNumber(values[prefix + "deadzone"], axis.deadzone)) {
            error = "Invalid profile entry for " + std::string(channelNames[i]);
            return std::nullopt;
        }
    }
    if (!validateProfile(profile, error))
        return std::nullopt;
    return profile;
}

bool saveProfile(const std::filesystem::path& path, const Profile& profile, std::string& error)
{
    if (!validateProfile(profile, error))
        return false;

    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.imbue(std::locale::classic());
    file << "# Chrome Axis calibration\nversion=1\n";
    const auto axes = channels(profile);
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto name = channelNames[i];
        const auto& axis = *axes[i];
        if (axis.deviceId.empty())
            continue;
        file << '\n' << name << ".device=" << axis.deviceId << '\n'
             << name << ".axis=" << axis.axis << '\n'
             << name << ".neutral=" << axis.neutral << '\n'
             << name << ".negative=" << axis.negative << '\n'
             << name << ".positive=" << axis.positive << '\n'
             << name << ".deadzone=" << axis.deadzone << '\n';
    }
    file.close();

    if (file && MoveFileExW(temporary.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "Cannot write the calibration file. Check the destination and its permissions";
    return false;
}

ControlReader::ControlReader(const Profile& profile)
    : profile_(profile), combinedPedals_(sameAxis(profile.throttle, profile.brake))
{
    const auto axes = channels(profile_);
    for (std::size_t i = 0; i < axes.size(); ++i) {
        std::size_t owner = 0;
        while (owner < i && axes[owner]->deviceId != axes[i]->deviceId)
            ++owner;
        sampleOwners_[i] = owner;
    }
    reset();
}

void ControlReader::reset() noexcept
{
    neutralPolls_.fill(0);
    const auto axes = channels(profile_);
    for (std::size_t i = 0; i < axes.size(); ++i) {
        axisStatuses_[i] = axes[i]->deviceId.empty() ? AxisStatus::unassigned : AxisStatus::deviceUnavailable;
    }
}

ControlState ControlReader::poll(DirectInput& input)
{
    const auto axes = channels(profile_);
    std::array<std::optional<DeviceSample>, 3> storage;
    std::array<const DeviceSample*, 3> samples{};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (axes[i]->deviceId.empty())
            continue;
        const std::size_t owner = sampleOwners_[i];
        if (owner == i)
            storage[owner] = input.sample(axes[i]->deviceId);
        if (storage[owner])
            samples[i] = &*storage[owner];
    }
    return update(samples);
}

ControlState ControlReader::update(const std::array<const DeviceSample*, 3>& samples)
{
    const auto axes = channels(profile_);
    ControlState state;
    const std::array outputs{&state.values.steering, &state.values.throttle, &state.values.brake};
    const std::array readiness{&state.steeringReady, &state.throttleReady, &state.brakeReady};

    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& calibration = *axes[i];
        if (calibration.deviceId.empty())
            continue;
        const auto* sample = samples[i];
        if (!sample || calibration.axis >= axisCount || !sample->axesPresent[calibration.axis]) {
            neutralPolls_[i] = 0;
            axisStatuses_[i] = sample ? AxisStatus::axisUnavailable : AxisStatus::deviceUnavailable;
            continue;
        }
        const long raw = sample->axes[calibration.axis];
        if (neutralPolls_[i] < requiredNeutralPolls) {
            axisStatuses_[i] = AxisStatus::waitingForNeutral;
            if (!isNeutral(calibration, raw, i == 0, combinedPedals_)) {
                neutralPolls_[i] = 0;
                continue;
            }
            if (++neutralPolls_[i] < requiredNeutralPolls)
                continue;
        }
        *readiness[i] = true;
        *outputs[i] = normalize(calibration, raw, i == 0);
        axisStatuses_[i] = AxisStatus::armed;
    }
    return state;
}

} // namespace chrome_axis
