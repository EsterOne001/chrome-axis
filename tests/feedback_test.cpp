#include "chrome_axis/feedback.hpp"

#include <windows.h>

#include <cmath>
#include <fstream>
#include <limits>

int main()
{
    using namespace chrome_axis;
    FeedbackSettings settings;
    WheelFeedback feedback;
    FeedbackSample motion{0.8f, 20.0f, 0.0f, 1600.0f};
    if (feedback.update(settings, motion, 0.02f) != 0.0f)
        return 1;
    settings.enabled = true;
    settings.strength = maxFeedbackPercent;

    const auto settle = [&](const FeedbackSample& sample) {
        feedback.reset();
        float value{};
        for (int i = 0; i < 30; ++i) {
            value = feedback.update(settings, sample, 0.02f);
        }
        return value;
    };
    const float moving = settle(motion);
    motion.forwardSpeed = 0.0f;
    const float stopped = settle(motion);
    if (!(moving < stopped && stopped < 0.0f))
        return 2;
    motion.forwardSpeed = 20.0f;
    motion.mass = 800.0f;
    if (!(settle(motion) > moving))
        return 3;
    motion.sideSpeed = 0.0f;
    motion.steering = 0.0f;
    motion.impactSpeed = 22.0f;
    motion.impactSide = -0.7f;
    const float impact = settle(motion);
    if (!(impact < 0.0f && std::abs(impact) <= 0.35f))
        return 4;
    settings.invert = true;
    if (std::abs(settle(motion) + impact) > 0.0001f)
        return 5;
    settings.impacts = false;
    if (settle(motion) != 0.0f)
        return 6;
    settings.impacts = true;
    motion.impactAge = 0.26f;
    if (settle(motion) != 0.0f)
        return 7;
    motion.forwardSpeed = std::numeric_limits<float>::infinity();
    if (feedback.update(settings, motion, 0.02f) != 0.0f)
        return 8;

    const auto path = std::filesystem::current_path()
        / (L"feedback-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    const bool defaults = loadFeedbackSettings(path, error) == FeedbackSettings{};
    const bool roundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\n";
    }
    const bool rejected = !loadFeedbackSettings(path, error) && !error.empty();
    std::filesystem::remove(path);
    return defaults && roundTrip && rejected ? 0 : 9;
}
