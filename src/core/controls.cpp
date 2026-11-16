#include "chrome_axis/controls.hpp"

#include "settings_replace.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr std::array<std::string_view, 3> channelNames{"steering", "throttle", "brake"};
constexpr std::array responses{&Profile::steeringResponse, &Profile::throttleResponse, &Profile::brakeResponse};
constexpr unsigned int requiredNeutralPolls = 3;
// Match the diagnostic reading, which is measured after the configured deadzone.
constexpr float steeringRearmTolerance = 0.015f;

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

double axisPosition(const AxisCalibration& calibration, long raw, bool steering)
{
    const double delta = distance(raw, calibration.neutral);
    const double positive = distance(calibration.positive, calibration.neutral);
    const double position = delta / positive;
    return steering && position < 0.0 ? -delta / distance(calibration.negative, calibration.neutral) : position;
}

float applyDeadzone(float travel, float deadzone)
{
    const float value = std::max(0.0f, std::abs(travel) - deadzone) / (1.0f - deadzone);
    return std::copysign(value, travel);
}

bool pedalAtRest(double position, float deadzone, bool combinedPedals)
{
    // A dedicated pedal can return beyond its measured rest position and still produce zero.
    // Shared pedals must stay near the common center so neither is held during arming.
    return (combinedPedals ? std::abs(position) : position) <= deadzone;
}

float applyResponse(float travel, float deadzone, const AxisResponse& response, bool steering) noexcept
{
    const float saturation = static_cast<float>(response.saturation) / 100.0f;
    const float position = std::clamp((std::abs(travel) - deadzone) / (saturation - deadzone), 0.0f, 1.0f);
    const float progressivity = static_cast<float>(response.progressivity) / 100.0f;
    // Positive steering sensitivity sharpens the centre. Positive pedal progressivity softens the start.
    const float adjusted = position + (steering ? progressivity : -progressivity) * position * (1.0f - position);
    return std::copysign(adjusted, travel);
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

float PedalStabilizer::update(float pedal, Clock::time_point sampledAt) noexcept
{
    constexpr float fastMovement = 0.035f;
    constexpr float noiseBand = 0.012f;
    const float movement = std::abs(pedal - output_);
    if (!previousSample_ || pedal == 0.0f || pedal == 1.0f || movement >= fastMovement) {
        output_ = pedal;
    } else if (movement > 0.0f) {
        // At the USB reference cadence of 5 ms the base filter retains 78% of the
        // distance. Keep the same decay until the input changes, independent of
        // how many times the same target is sampled.
        if (pedal != input_) {
            const float retention = 0.78f * std::min(1.0f, (fastMovement - movement) / (fastMovement - noiseBand));
            logRetention_ = std::log(retention);
        }
        const float periods = std::chrono::duration<float, std::milli>(sampledAt - *previousSample_).count() / 5.0f;
        const float fraction = -std::expm1(logRetention_ * std::max(0.0f, periods));
        output_ = std::lerp(output_, pedal, fraction);
    }
    input_ = pedal;
    previousSample_ = sampledAt;
    return output_;
}

bool validateAxisResponse(const AxisResponse& response) noexcept
{
    return response.progressivity >= minimumResponseProgressivity && response.progressivity <= maximumResponseProgressivity
        && response.saturation >= minimumResponseSaturation && response.saturation <= maximumResponseSaturation;
}

AxisPreview previewAxisResponse(ResponseAxis axis, const AxisCalibration& calibration, long raw,
                               const AxisResponse& response) noexcept
{
    const bool steering = axis == ResponseAxis::steering;
    const double positive = distance(calibration.positive, calibration.neutral);
    const double negative = distance(calibration.negative, calibration.neutral);
    if (!validateAxisResponse(response) || positive == 0.0 || (steering && positive * negative >= 0.0) ||
        !(calibration.deadzone >= 0.0f && calibration.deadzone < static_cast<float>(response.saturation) / 100.0f))
        return {};
    const auto travel = static_cast<float>(std::clamp(axisPosition(calibration, raw, steering), steering ? -1.0 : 0.0, 1.0));
    return {travel, applyResponse(travel, calibration.deadzone, response, steering)};
}

bool validateBrakeResponse(const BrakeResponse& response) noexcept
{
    return validateAxisResponse(response);
}

BrakePreview previewBrake(const AxisCalibration& calibration, long raw, const BrakeResponse& response) noexcept
{
    return previewAxisResponse(ResponseAxis::brake, calibration, raw, response);
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
        const float minimumDeadzone = i == 0 ? 0.0f : minimumPedalDeadzone;
        if (!isDeviceId(axis.deviceId) || axis.axis >= axisCount || !validRange
            || !std::isfinite(axis.deadzone) || axis.deadzone < minimumDeadzone || axis.deadzone > maximumAxisDeadzone) {
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
    for (std::size_t i = 0; i < responses.size(); ++i) {
        if (!validateAxisResponse(profile.*responses[i]) || (i == 0 && (profile.*responses[i]).stabilization)) {
            error = "Invalid " + std::string(channelNames[i]) + " response settings";
            return false;
        }
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
    for (std::size_t i = 0; i < responses.size(); ++i) {
        auto& response = profile.*responses[i];
        const std::string prefix = std::string(channelNames[i]) + "_response.";
        const auto progressivity = values.find(prefix + "progressivity");
        const auto saturation = values.find(prefix + "saturation");
        const auto stabilization = values.find(prefix + "stabilization");
        if ((progressivity != values.end() && !parseNumber(progressivity->second, response.progressivity))
            || (saturation != values.end() && !parseNumber(saturation->second, response.saturation))
            || (stabilization != values.end() && stabilization->second != "0" && stabilization->second != "1")) {
            error = "Invalid " + std::string(channelNames[i]) + " response settings";
            return std::nullopt;
        }
        response.stabilization = stabilization != values.end() && stabilization->second == "1";
    }
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

    const auto temporary = detail::stagedSettingsPath(path);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.imbue(std::locale::classic());
    file.precision(std::numeric_limits<float>::max_digits10);
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
    for (std::size_t i = 0; i < responses.size(); ++i) {
        const auto& response = profile.*responses[i];
        if (response != AxisResponse{}) {
            file << '\n' << channelNames[i] << "_response.progressivity=" << response.progressivity << '\n'
                 << channelNames[i] << "_response.saturation=" << response.saturation << '\n';
            if (response.stabilization)
                file << channelNames[i] << "_response.stabilization=1\n";
        }
    }
    file.close();

    if (file && detail::replaceSettingsFile(temporary, path)) {
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
    lastSequences_.fill(0);
    for (auto& stabilizer : pedalStabilizers_)
        stabilizer.reset();
    const auto axes = channels(profile_);
    for (std::size_t i = 0; i < axes.size(); ++i) {
        axisStatuses_[i] = axes[i]->deviceId.empty() ? AxisStatus::unassigned : AxisStatus::deviceUnavailable;
    }
}

ControlState ControlReader::poll(DeviceSampleSource& input, Readings* readings)
{
    return poll(input, readings, nullptr);
}

ControlReader::CalibrationReadings ControlReader::review(DeviceSampleSource& input)
{
    CalibrationReadings readings;
    poll(input, nullptr, &readings);
    return readings;
}

ControlState ControlReader::poll(DeviceSampleSource& input, Readings* readings, CalibrationReadings* calibration)
{
    const auto axes = channels(profile_);
    std::array<std::optional<DeviceSample>, 3> storage;
    std::array<const DeviceSample*, 3> samples{};
    std::array<DeviceReadStatus, 3> statuses{};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (axes[i]->deviceId.empty())
            continue;
        const std::size_t owner = sampleOwners_[i];
        if (owner == i)
            storage[owner] = input.sample(axes[i]->deviceId, {}, &statuses[owner]);
        else
            statuses[i] = statuses[owner];
        if (storage[owner])
            samples[i] = &*storage[owner];
    }
    return update(samples, readings, statuses, calibration);
}

ControlState ControlReader::update(const std::array<const DeviceSample*, 3>& samples, Readings* readings,
                                  const std::array<DeviceReadStatus, 3>& statuses, CalibrationReadings* review,
                                  PedalStabilizer::Clock::time_point sampledAt)
{
    if (readings)
        *readings = {};
    if (review)
        *review = {};
    const auto axes = channels(profile_);
    ControlState state;
    const std::array outputs{&state.values.steering, &state.values.throttle, &state.values.brake};
    const std::array readiness{&state.steeringReady, &state.throttleReady, &state.brakeReady};

    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& calibration = *axes[i];
        if (calibration.deviceId.empty())
            continue;
        state.assigned.set(i);
        const auto* sample = samples[i];
        const bool fresh = sample && (!sample->sequence || lastSequences_[i] != sample->sequence);
        if (sample)
            lastSequences_[i] = sample->sequence;
        if (i > 0 && (!sample || calibration.axis >= axisCount || !sample->axesPresent[calibration.axis] ||
                      statuses[i] == DeviceReadStatus::resumed))
            pedalStabilizers_[i - 1].reset();
        if (statuses[i] == DeviceReadStatus::resumed)
            neutralPolls_[i] = 0;
        if ((!sample && statuses[i] == DeviceReadStatus::pending) ||
            (sample && calibration.axis < axisCount && sample->axesPending[calibration.axis])) {
            if (neutralPolls_[i] < requiredNeutralPolls)
                neutralPolls_[i] = 0;
            axisStatuses_[i] = AxisStatus::temporarilyUnavailable;
            continue;
        }
        if (!sample || calibration.axis >= axisCount || !sample->axesPresent[calibration.axis]) {
            neutralPolls_[i] = 0;
            axisStatuses_[i] = sample ? AxisStatus::axisUnavailable : AxisStatus::deviceUnavailable;
            continue;
        }
        const long raw = sample->axes[calibration.axis];
        const bool steering = i == 0;
        const bool brake = i == 2;
        const auto& response = profile_.*responses[i];
        const double position = axisPosition(calibration, raw, steering);
        const auto travel = static_cast<float>(std::clamp(position, steering ? -1.0 : 0.0, 1.0));
        const float calibrated = applyDeadzone(travel, calibration.deadzone);
        const auto output = [&] {
            const float adjusted = (response.progressivity == 0 && response.saturation == maximumResponseSaturation)
                ? calibrated : applyResponse(travel, calibration.deadzone, response, steering);
            return response.stabilization && !steering
                ? pedalStabilizers_[i - 1].update(adjusted, sample->sampledAt == DeviceReadClock::time_point{}
                    ? sampledAt : sample->sampledAt) : adjusted;
        };
        if (review) {
            review->travel[i] = travel;
            if (brake)
                review->adjustedBrake = output();
        }
        if (readings)
            (*readings)[i] = calibrated;
        if (neutralPolls_[i] < requiredNeutralPolls) {
            axisStatuses_[i] = AxisStatus::waitingForNeutral;
            const bool neutral = steering ? std::abs(calibrated) <= steeringRearmTolerance
                : pedalAtRest(position, calibration.deadzone, combinedPedals_);
            if (!neutral) {
                neutralPolls_[i] = 0;
                continue;
            }
            if (fresh)
                ++neutralPolls_[i];
            if (neutralPolls_[i] < requiredNeutralPolls)
                continue;
        }
        *readiness[i] = true;
        if (steering)
            state.calibratedSteering = calibrated;
        *outputs[i] = review && brake ? *review->adjustedBrake : output();
        state.sampledAt[i] = sample->sampledAt;
        axisStatuses_[i] = AxisStatus::armed;
    }
    return state;
}

} // namespace chrome_axis
