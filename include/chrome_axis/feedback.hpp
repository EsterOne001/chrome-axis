#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace chrome_axis {

inline constexpr wchar_t feedbackFilename[] = L"force_feedback.ini";
inline constexpr int maxFeedbackGainPercent = 200;
inline constexpr int minFeedbackCenteringPercent = 50;
inline constexpr int maxFeedbackCenteringPercent = 250;

struct FeedbackSettings {
    bool enabled{};
    int strength{12};
    bool impacts{true};
    bool invert{};
    int centering{100};

    bool operator==(const FeedbackSettings&) const = default;
};

std::optional<FeedbackSettings> loadFeedbackSettings(const std::filesystem::path& file, std::string& error);
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

class WheelFeedback {
public:
    // Returns a signed fraction of DirectInput's nominal force, in calibrated steering coordinates.
    float update(const FeedbackSettings& settings, const FeedbackSample& sample, float seconds,
                 bool* clipped = nullptr) noexcept;
    void reset() noexcept;

private:
    float previousSteering_{};
    float smoothedForce_{};
    bool initialized_{};
};

} // namespace chrome_axis
