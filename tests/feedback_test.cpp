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
    settings.strength = 35;

    const auto settle = [&](const FeedbackSample& sample) {
        feedback.reset();
        float value{};
        for (int i = 0; i < 30; ++i) {
            value = feedback.update(settings, sample, 0.02f);
        }
        return value;
    };
    const float legacyMoving = settle(motion);
    settings.strength = 100;
    const float moving = settle(motion);
    if (!(moving < legacyMoving) || std::abs(legacyMoving - moving * 0.35f) > 0.0001f)
        return 10;
    settings.strength = 200;
    if (std::abs(settle(motion) - moving * 2.0f) > 0.0001f)
        return 13;
    settings.strength = 100;
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
    if (!(impact < -0.35f && impact >= -1.0f))
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

    // Centering and impact can add up to more than the nominal force.
    const FeedbackSample hardImpact{-1.0f, 120.0f, 0.0f, 3000.0f, 22.0f, 1.0f};
    if (settle(hardImpact) != -1.0f)
        return 11;
    settings.strength = 200;
    if (settle(hardImpact) != -1.0f)
        return 14;
    settings.invert = false;
    if (settle(hardImpact) != 1.0f)
        return 12;

    const auto path = std::filesystem::current_path()
        / (L"feedback-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    const auto defaults = loadFeedbackSettings(path, error);
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=35\nimpacts=0\ninvert=1\n";
    }
    const bool legacy = loadFeedbackSettings(path, error) == FeedbackSettings{true, 35, false, true};
    const bool roundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessive{true, 201, true, false};
    const bool rejectedSave = !saveFeedbackSettings(path, excessive, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=201\nimpacts=1\ninvert=0\n";
    }
    const bool rejected = !loadFeedbackSettings(path, error) && !error.empty();
    std::filesystem::remove(path);
    return defaults && !defaults->enabled && defaults->strength == 12 && legacy && roundTrip && rejectedSave && rejected
        ? 0 : 9;
}
