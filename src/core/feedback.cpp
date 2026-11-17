#include "chrome_axis/feedback.hpp"
#include "chrome_axis/controls.hpp"

#include "settings_read.hpp"
#include "settings_replace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr std::array<std::string_view, 9> keys{
    "version", "enabled", "strength", "impacts", "invert", "centering", "impact_strength", "# low_speed_effort", "# damping"
};
constexpr std::size_t strengthKey = 2;
constexpr std::size_t centeringKey = 5;
constexpr std::size_t impactStrengthKey = 6;
constexpr std::size_t lowSpeedEffortKey = 7;
constexpr std::size_t dampingKey = 8;

bool valid(const FeedbackSettings& settings) noexcept
{
    return settings.strength >= 1 && settings.strength <= maxFeedbackGainPercent &&
        settings.centering >= minFeedbackCenteringPercent && settings.centering <= maxFeedbackCenteringPercent &&
        settings.impactStrength >= 0 && settings.impactStrength <= maxFeedbackImpactPercent &&
        settings.lowSpeedEffort >= 0 && settings.lowSpeedEffort <= maxFeedbackTuningPercent &&
        settings.damping >= 0 && settings.damping <= maxFeedbackTuningPercent;
}

float normalizedMotion(float value, float fullScale) noexcept
{
    return std::isfinite(value) ? std::clamp(value / fullScale, -1.0f, 1.0f) : 0.0f;
}

float centeredPulse(float phase) noexcept
{
    // Opposite lobes give a frontal impact or landing no net steering direction.
    return 4.0f * phase * (1.0f - phase) * std::sin(phase * 2.0f * std::numbers::pi_v<float>);
}

} // namespace

std::uint64_t feedbackMeasurementSignature(const FeedbackSettings& settings, const Profile& profile) noexcept
{
    std::uint64_t signature = 14695981039346656037ull;
    const auto add = [&](std::uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
    add(settings.enabled);
    add(settings.strength);
    add(settings.impacts);
    add(settings.invert);
    add(settings.centering);
    add(settings.impactStrength);
    add(settings.lowSpeedEffort);
    add(settings.damping);
    for (const auto* axis : {&profile.steering, &profile.throttle, &profile.brake}) {
        add(axis->deviceId.size());
        for (const auto byte : axis->deviceId)
            add(static_cast<unsigned char>(byte));
        add(axis->axis);
        add(axis->neutral);
        add(axis->negative);
        add(axis->positive);
        add(std::bit_cast<std::uint32_t>(axis->deadzone));
    }
    for (const auto* response : {&profile.steeringResponse, &profile.throttleResponse, &profile.brakeResponse}) {
        add(response->progressivity);
        add(response->saturation);
        add(response->stabilization);
    }
    return signature ? signature : 1;
}

std::optional<FeedbackSettings> loadFeedbackSettings(const std::filesystem::path& path, std::string& error,
                                                    DWORD* readError)
{
    error.clear();
    auto text = detail::readSettingsText(path, 1024);
    if (readError)
        *readError = text.error;
    if (text.error == ERROR_FILE_NOT_FOUND || text.error == ERROR_PATH_NOT_FOUND)
        return FeedbackSettings{};

    if (text.error || text.invalidSize) {
        error = text.error ? "Force feedback settings are unreadable." : "Invalid force feedback settings.";
        return std::nullopt;
    }
    std::istringstream file(std::move(text.contents));
    std::array<int, keys.size()> values{};
    values[centeringKey] = FeedbackSettings{}.centering;
    values[impactStrengthKey] = FeedbackSettings{}.impactStrength;
    values[lowSpeedEffortKey] = FeedbackSettings{}.lowSpeedEffort;
    values[dampingKey] = FeedbackSettings{}.damping;
    std::array<bool, keys.size()> seen{};
    std::string line;
    while (std::getline(file, line)) {
        if (line.ends_with('\r'))
            line.pop_back();
        if (line.empty())
            continue;
        const auto separator = line.find('=');
        const auto key = std::string_view(line).substr(0, separator);
        // Keep the added settings in comments for apps that load files written by this version.
        if (line.front() == '#' && key != keys[lowSpeedEffortKey] && key != keys[dampingKey])
            continue;
        const auto match = std::ranges::find(keys, key);
        if (separator == std::string::npos || match == keys.end()) {
            error = "Invalid force feedback settings.";
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(match - keys.begin());
        const char* end = line.data() + line.size();
        const auto parsed = std::from_chars(line.data() + separator + 1, end, values[index]);
        const bool flag = index != strengthKey && index < centeringKey;
        if (seen[index] || parsed.ec != std::errc{} || parsed.ptr != end ||
            (flag && (values[index] < 0 || values[index] > 1))) {
            error = "Invalid force feedback settings.";
            return std::nullopt;
        }
        seen[index] = true;
    }
    const FeedbackSettings settings{values[1] == 1, values[strengthKey], values[3] == 1,
                                    values[4] == 1, values[centeringKey], values[impactStrengthKey],
                                    values[lowSpeedEffortKey], values[dampingKey]};
    const auto requiredEnd = seen.begin() + centeringKey;
    if (file.bad() || values[0] != 1 || !valid(settings) || std::find(seen.begin(), requiredEnd, false) != requiredEnd) {
        error = "Invalid force feedback settings.";
        return std::nullopt;
    }
    return settings;
}

bool saveFeedbackSettings(const std::filesystem::path& path, const FeedbackSettings& settings, std::string& error)
{
    error.clear();
    if (!valid(settings)) {
        error = "A force feedback adjustment is out of range.";
        return false;
    }
    std::error_code status;
    std::filesystem::create_directories(path.parent_path(), status);
    if (status) {
        error = "Could not open the force feedback settings directory.";
        return false;
    }
    const auto temporary = detail::stagedSettingsPath(path);
    std::ofstream file(temporary, std::ios::trunc);
    file << "version=1\nenabled=" << settings.enabled << "\nstrength=" << settings.strength
         << "\nimpacts=" << settings.impacts << "\ninvert=" << settings.invert << '\n';
    if (settings.centering != FeedbackSettings{}.centering)
        file << "centering=" << settings.centering << '\n';
    if (settings.impactStrength != FeedbackSettings{}.impactStrength)
        file << "impact_strength=" << settings.impactStrength << '\n';
    if (settings.lowSpeedEffort != FeedbackSettings{}.lowSpeedEffort)
        file << "# low_speed_effort=" << settings.lowSpeedEffort << '\n';
    if (settings.damping != FeedbackSettings{}.damping)
        file << "# damping=" << settings.damping << '\n';
    file.close();
    if (!file || !detail::replaceSettingsFile(temporary, path)) {
        std::filesystem::remove(temporary, status);
        error = "Could not save the force feedback settings.";
        return false;
    }
    return true;
}

FeedbackOutput WheelFeedback::update(const FeedbackSettings& settings, const FeedbackSample& sample,
                                     float seconds) noexcept
{
    if (!settings.enabled || !valid(settings) || !std::isfinite(seconds) || seconds <= 0.0f ||
        !std::isfinite(sample.steering) || !std::isfinite(sample.forwardSpeed) ||
        !std::isfinite(sample.sideSpeed) || !std::isfinite(sample.mass)) {
        reset();
        return {};
    }
    seconds = std::clamp(seconds, 0.010f, 0.055f);
    const float steering = std::clamp(sample.steering, -1.0f, 1.0f);
    const float speed = std::abs(sample.forwardSpeed);
    const float speedFactor = speed / (speed + 13.0f);
    const float slip = std::clamp(sample.sideSpeed / std::max(speed, 3.0f), -1.0f, 1.0f);
    const float weight = std::clamp(sample.mass / 1700.0f, 0.65f, 1.35f);
    const float wheelRate = initialized_ ? std::clamp((steering - previousSteering_) / seconds, -5.0f, 5.0f) : 0.0f;
    previousSteering_ = steering;
    initialized_ = true;

    // Body slip and the game's skid flag estimate overall grip. Airborne motion
    // doesn't tell us about tire grip, so keep the last estimate until contact.
    if (!sample.airborne) {
        constexpr float slipStart = 0.04f;
        constexpr float fullSlip = 0.65f;
        const float slipLevel = std::clamp((std::abs(slip) - slipStart) / (fullSlip - slipStart), 0.0f, 1.0f);
        const float slipLoss = 0.65f * slipLevel * slipLevel * (3.0f - 2.0f * slipLevel);
        const float targetGrip = (1.0f - slipLoss) * (sample.skidding ? 0.60f : 1.0f);
        // Ease loss of grip and bring back the load more slowly as traction returns.
        const float timeConstant = targetGrip < grip_ ? 0.10f : 0.25f;
        grip_ = std::lerp(grip_, targetGrip, 1.0f - std::exp(-seconds / timeConstant));
    }
    const float contact = sample.airborne ? 0.16f : 1.0f;
    const float centeringGain = static_cast<float>(settings.centering) / 100.0f;
    const float turnLoad = 0.24f * normalizedMotion(sample.lateralAcceleration, 8.0f) +
        0.045f * normalizedMotion(sample.yawRate, 1.25f);
    const float parkingWeight = 4.0f / (speed + 4.0f);
    const float lowSpeedAdjustment = 0.10f * parkingWeight * (settings.lowSpeedEffort / 100.0f - 1.0f);
    const float steeringLoad = (0.10f + lowSpeedAdjustment + 0.72f * speedFactor) * steering;
    const float corneringLoad = sample.forwardSpeed > 0.0f ? speedFactor * turnLoad : 0.0f;
    const float centering = -(steeringLoad + corneringLoad) * weight * centeringGain * contact * grip_;
    const float damping = -(0.015f + 0.035f * speedFactor) * wheelRate * (settings.damping / 100.0f);
    const float lateral = -0.11f * slip * speedFactor * contact * grip_;
    smoothedForce_ += std::min(seconds * 12.0f, 1.0f) * (centering + damping + lateral - smoothedForce_);

    float transient = 0.0f;
    if (settings.impacts && std::isfinite(sample.impactSpeed) && std::isfinite(sample.impactSide) &&
        sample.impactSpeed >= 1.5f && sample.impactAge >= 0.0f && sample.impactAge < 0.24f) {
        const float phase = sample.impactAge / 0.24f;
        const float strength = std::clamp((sample.impactSpeed - 1.5f) / 12.0f, 0.0f, 1.0f);
        const float side = std::clamp(sample.impactSide, -1.0f, 1.0f);
        const float kick = side * (1.0f - phase) * (1.0f - phase) *
            std::cos(phase * 4.0f * std::numbers::pi_v<float>);
        const float frontal = (1.0f - std::abs(side)) * 0.24f * centeredPulse(phase);
        transient = 0.66f * strength * (kick + frontal);
    } else if (settings.impacts && !sample.airborne && std::isfinite(sample.landingSpeed) &&
               sample.landingSpeed > 0.8f && sample.landingAge >= 0.0f && sample.landingAge < 0.18f) {
        const float strength = std::clamp((sample.landingSpeed - 0.8f) / 6.0f, 0.0f, 1.0f);
        transient = 0.20f * strength * centeredPulse(sample.landingAge / 0.18f);
    }
    const float gain = static_cast<float>(settings.strength) / 100.0f;
    const float signedGain = settings.invert ? -gain : gain;
    const float impactGain = static_cast<float>(settings.impactStrength) / 100.0f;
    const float continuousForce = signedGain * smoothedForce_;
    const float requestedForce = continuousForce + signedGain * transient * impactGain;
    const float force = std::clamp(requestedForce, -1.0f, 1.0f);
    auto saturation = FeedbackSaturation::none;
    if (requestedForce != force) {
        // Attribute the excess to the steady load if it would already clip in this direction.
        saturation = continuousForce * force > 1.0f ? FeedbackSaturation::continuous : FeedbackSaturation::transient;
    }
    return {requestedForce, force, saturation, continuousForce};
}

void WheelFeedback::reset() noexcept
{
    previousSteering_ = 0.0f;
    smoothedForce_ = 0.0f;
    grip_ = 1.0f;
    initialized_ = false;
}

} // namespace chrome_axis
