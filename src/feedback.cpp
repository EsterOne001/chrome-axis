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

constexpr std::array<std::string_view, 5> keys{"version", "enabled", "strength", "impacts", "invert"};

bool valid(const FeedbackSettings& settings) noexcept
{
    return settings.strength >= 1 && settings.strength <= maxFeedbackPercent;
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
        if (seen[index] || parsed.ec != std::errc{} || parsed.ptr != end ||
            (index != 2 && (values[index] < 0 || values[index] > 1))) {
            error = "Invalid force feedback settings.";
            return std::nullopt;
        }
        seen[index] = true;
    }
    const FeedbackSettings settings{values[1] == 1, values[2], values[3] == 1, values[4] == 1};
    if (file.bad() || values[0] != 1 || !valid(settings) || std::ranges::find(seen, false) != seen.end()) {
        error = "Invalid force feedback settings.";
        return std::nullopt;
    }
    return settings;
}

bool saveFeedbackSettings(const std::filesystem::path& path, const FeedbackSettings& settings, std::string& error)
{
    error.clear();
    if (!valid(settings)) {
        error = "Force feedback strength is out of range.";
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
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, status);
        error = "Could not save the force feedback settings.";
        return false;
    }
    return true;
}

float WheelFeedback::update(const FeedbackSettings& settings, const FeedbackSample& sample, float seconds) noexcept
{
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

    const float centering = -(0.10f + 0.72f * speedFactor) * weight * (1.0f - 0.65f * std::abs(slip)) * steering;
    const float damping = -(0.015f + 0.035f * speedFactor) * wheelRate;
    const float lateral = -0.11f * slip * speedFactor;
    smoothedForce_ += std::min(seconds * 12.0f, 1.0f) * (centering + damping + lateral - smoothedForce_);

    float impact = 0.0f;
    if (settings.impacts && std::isfinite(sample.impactSpeed) && std::isfinite(sample.impactSide) &&
        sample.impactSpeed >= 1.5f && sample.impactAge >= 0.0f && sample.impactAge < 0.24f) {
        const float phase = sample.impactAge / 0.24f;
        const float strength = std::clamp((sample.impactSpeed - 1.5f) / 12.0f, 0.0f, 1.0f);
        const float direction = sample.impactSide < 0.0f ? -1.0f : 1.0f;
        impact = 0.66f * strength * (1.0f - phase) * (1.0f - phase) * direction
            * std::cos(phase * 4.0f * std::numbers::pi_v<float>);
    }
    const float gain = static_cast<float>(settings.strength) / 100.0f;
    return (settings.invert ? -gain : gain) * std::clamp(smoothedForce_ + impact, -1.0f, 1.0f);
}

void WheelFeedback::reset() noexcept
{
    previousSteering_ = 0.0f;
    smoothedForce_ = 0.0f;
    initialized_ = false;
}

} // namespace chrome_axis
