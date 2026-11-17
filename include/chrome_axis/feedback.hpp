#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace chrome_axis {

inline constexpr wchar_t feedbackFilename[] = L"force_feedback.ini";
inline constexpr int maxFeedbackGainPercent = 200;
inline constexpr int minFeedbackCenteringPercent = 50;
inline constexpr int maxFeedbackCenteringPercent = 250;
inline constexpr int maxFeedbackImpactPercent = 200;
inline constexpr int maxFeedbackTuningPercent = 200;

struct FeedbackSettings {
    bool enabled{};
    int strength{12};
    bool impacts{true};
    bool invert{};
    int centering{100};
    int impactStrength{100};
    int lowSpeedEffort{100};
    int damping{100};

    bool operator==(const FeedbackSettings&) const = default;
};

struct Profile;

// Settings and calibration that must match before reusing a gain recommendation.
std::uint64_t feedbackMeasurementSignature(const FeedbackSettings& settings, const Profile& profile) noexcept;

std::optional<FeedbackSettings> loadFeedbackSettings(const std::filesystem::path& file, std::string& error,
                                                    DWORD* readError = nullptr);
bool saveFeedbackSettings(const std::filesystem::path& file, const FeedbackSettings& settings, std::string& error);

struct FeedbackSample {
    float steering{};
    float forwardSpeed{};
    float sideSpeed{};
    float mass{1500.0f};
    float impactSpeed{};
    float impactSide{};
    float impactAge{};
    float lateralAcceleration{}; // m/s², positive toward the vehicle's right.
    float yawRate{}; // rad/s, positive for rightward rotation.
    float landingSpeed{}; // Downward speed before contact, in m/s.
    float landingAge{}; // Seconds since contact.
    bool skidding{};
    bool airborne{};
};

// A transient is a collision or landing needed to push the summed force past the limit.
enum class FeedbackSaturation { none, continuous, transient };

struct FeedbackOutput {
    // Signed relative to DirectInput's nominal force, in calibrated steering coordinates.
    float requestedForce{};
    float force{};
    FeedbackSaturation saturation{};
    float continuousForce{};
};

class WheelFeedback {
public:
    FeedbackOutput update(const FeedbackSettings& settings, const FeedbackSample& sample, float seconds) noexcept;
    void reset() noexcept;

private:
    float previousSteering_{};
    float smoothedForce_{};
    float grip_{1.0f};
    bool initialized_{};
};

} // namespace chrome_axis
