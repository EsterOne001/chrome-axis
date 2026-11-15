#include "chrome_axis/feedback.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numbers>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr std::array<std::string_view, 6> keys{"version", "enabled", "strength", "impacts", "invert", "centering"};
constexpr std::size_t strengthKey = 2;
constexpr std::size_t centeringKey = keys.size() - 1;

bool valid(const FeedbackSettings& settings) noexcept
{
    return settings.strength >= 1 && settings.strength <= maxFeedbackGainPercent &&
        settings.centering >= minFeedbackCenteringPercent && settings.centering <= maxFeedbackCenteringPercent;
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

std::optional<FeedbackSettings> loadFeedbackSettings(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::error_code status;
    if (!std::filesystem::exists(path, status) && !status)
        return FeedbackSettings{};

    std::ifstream file(path, std::ios::ate);
    if (!file || file.tellg() > 1024) {
        error = "Force feedback settings are unreadable.";
        return std::nullopt;
    }
    file.seekg(0);
    std::array<int, keys.size()> values{};
    values[centeringKey] = FeedbackSettings{}.centering;
    std::array<bool, keys.size()> seen{};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        const auto separator = line.find('=');
        const auto key = std::string_view(line).substr(0, separator);
        const auto match = std::ranges::find(keys, key);
        if (separator == std::string::npos || match == keys.end()) {
            error = "Invalid force feedback settings.";
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(match - keys.begin());
        const char* end = line.data() + line.size();
        const auto parsed = std::from_chars(line.data() + separator + 1, end, values[index]);
        const bool flag = index != strengthKey && index != centeringKey;
        if (seen[index] || parsed.ec != std::errc{} || parsed.ptr != end ||
            (flag && (values[index] < 0 || values[index] > 1))) {
            error = "Invalid force feedback settings.";
            return std::nullopt;
        }
        seen[index] = true;
    }
    const FeedbackSettings settings{values[1] == 1, values[strengthKey], values[3] == 1,
                                    values[4] == 1, values[centeringKey]};
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
        error = "Force feedback strength or centering is out of range.";
        return false;
    }
    std::error_code status;
    std::filesystem::create_directories(path.parent_path(), status);
    if (status) {
        error = "Could not open the force feedback settings directory.";
        return false;
    }
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream file(temporary, std::ios::trunc);
    file << "version=1\nenabled=" << settings.enabled << "\nstrength=" << settings.strength
         << "\nimpacts=" << settings.impacts << "\ninvert=" << settings.invert << '\n';
    if (settings.centering != FeedbackSettings{}.centering)
        file << "centering=" << settings.centering << '\n';
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, status);
        error = "Could not save the force feedback settings.";
        return false;
    }
    return true;
}

float WheelFeedback::update(const FeedbackSettings& settings, const FeedbackSample& sample, float seconds,
                            bool* clipped) noexcept
{
    if (clipped)
        *clipped = false;
    if (!settings.enabled || !valid(settings) || !std::isfinite(seconds) || seconds <= 0.0f ||
        !std::isfinite(sample.steering) || !std::isfinite(sample.forwardSpeed) ||
        !std::isfinite(sample.sideSpeed) || !std::isfinite(sample.mass)) {
        reset();
        return 0.0f;
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

    const float contact = sample.airborne ? 0.16f : 1.0f;
    const float tireGrip = sample.skidding ? 0.60f : 1.0f;
    const float centeringGain = static_cast<float>(settings.centering) / 100.0f;
    const float turnLoad = 0.24f * normalizedMotion(sample.lateralAcceleration, 8.0f) +
        0.045f * normalizedMotion(sample.yawRate, 1.25f);
    const float steeringLoad = (0.10f + 0.72f * speedFactor) * steering;
    const float corneringLoad = sample.forwardSpeed > 0.0f ? speedFactor * turnLoad : 0.0f;
    const float centering = -(steeringLoad + corneringLoad) * weight * centeringGain * contact * tireGrip *
        (1.0f - 0.65f * std::abs(slip));
    const float damping = -(0.015f + 0.035f * speedFactor) * wheelRate;
    const float lateral = -0.11f * slip * speedFactor * contact * tireGrip;
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
    const float force = (settings.invert ? -gain : gain) * (smoothedForce_ + transient);
    if (clipped)
        *clipped = std::abs(force) > 1.0f;
    return std::clamp(force, -1.0f, 1.0f);
}

void WheelFeedback::reset() noexcept
{
    previousSteering_ = 0.0f;
    smoothedForce_ = 0.0f;
    initialized_ = false;
}

} // namespace chrome_axis
