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
    bool clipped = true;
    if (feedback.update(settings, motion, 0.02f, &clipped) != 0.0f || clipped)
        return 1;
    settings.enabled = true;
    settings.strength = 35;

    const auto settle = [&](const FeedbackSample& sample, bool* saturated = nullptr) {
        feedback.reset();
        float value{};
        for (int i = 0; i < 30; ++i) {
            value = feedback.update(settings, sample, 0.02f, saturated);
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

    const FeedbackSample straight{0.30f, 20.0f, 0.0f, 1600.0f};
    auto turn = straight;
    turn.lateralAcceleration = 4.0f;
    turn.yawRate = 0.35f;
    const float straightForce = settle(straight);
    const float turnForce = settle(turn);
    if (!(turnForce < straightForce - 0.02f))
        return 15;
    turn.skidding = true;
    const float skidForce = settle(turn);
    if (!(skidForce > turnForce && skidForce < 0.0f))
        return 16;
    turn.skidding = false;
    settle(turn);
    turn.airborne = true;
    const float firstAirborne = feedback.update(settings, turn, 0.02f);
    const float airborneForce = settle(turn);
    if (!(turnForce < firstAirborne && firstAirborne < airborneForce && airborneForce < 0.0f))
        return 17;
    settings.centering = 150;
    if (std::abs(settle(straight, &clipped) - 1.5f * straightForce) > 0.0001f || clipped)
        return 18;
    settings.centering = 100;
    turn.airborne = false;
    turn.steering = -turn.steering;
    turn.lateralAcceleration = -turn.lateralAcceleration;
    turn.yawRate = -turn.yawRate;
    if (std::abs(settle(turn) + turnForce) > 0.0001f)
        return 19;

    motion.sideSpeed = 0.0f;
    motion.steering = 0.0f;
    motion.impactSpeed = 22.0f;
    motion.impactSide = -0.7f;
    const float impact = settle(motion);
    if (!(impact < -0.35f && impact >= -1.0f))
        return 4;
    motion.impactSide = 0.35f;
    if (std::abs(settle(motion) + 0.5f * impact) > 0.0001f)
        return 20;
    motion.impactSide = 0.0f;
    if (settle(motion) != 0.0f)
        return 21;
    motion.impactAge = 0.06f;
    const float frontal = settle(motion);
    motion.impactAge = 0.18f;
    if (!(frontal > 0.0f) || std::abs(settle(motion) + frontal) > 0.0001f)
        return 22;
    motion.impactAge = 0.0f;
    motion.impactSide = -0.7f;
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

    FeedbackSample landing;
    landing.landingSpeed = 6.0f;
    landing.landingAge = 0.045f;
    const float landingForce = settle(landing);
    landing.landingAge = 0.135f;
    if (!(landingForce < 0.0f && landingForce > impact) ||
        std::abs(settle(landing) + landingForce) > 0.0001f)
        return 23;
    landing.airborne = true;
    if (settle(landing) != 0.0f)
        return 24;
    landing.airborne = false;
    landing.impactSpeed = motion.impactSpeed;
    landing.impactSide = motion.impactSide;
    if (std::abs(settle(landing) + impact) > 0.0001f)
        return 25;

    motion.forwardSpeed = std::numeric_limits<float>::infinity();
    if (feedback.update(settings, motion, 0.02f) != 0.0f)
        return 8;

    // Centering and impact can add up to more than the nominal force.
    const FeedbackSample hardImpact{-1.0f, 120.0f, 0.0f, 3000.0f, 22.0f, 1.0f};
    if (settle(hardImpact, &clipped) != -1.0f || !clipped)
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
    settings.centering = 165;
    const bool roundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessive{true, 201, true, false};
    const bool rejectedSave = !saveFeedbackSettings(path, excessive, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessiveCentering{true, 200, true, false, 251};
    const bool rejectedCentering = !saveFeedbackSettings(path, excessiveCentering, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\ncentering=49\n";
    }
    const bool rejected = !loadFeedbackSettings(path, error) && !error.empty();
    std::filesystem::remove(path);
    return defaults == FeedbackSettings{} && legacy && roundTrip && rejectedSave && rejectedCentering && rejected ? 0 : 9;
}
