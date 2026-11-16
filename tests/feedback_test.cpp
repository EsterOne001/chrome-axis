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
    const auto disabled = feedback.update(settings, motion, 0.02f);
    if (disabled.force != 0.0f || disabled.requestedForce != 0.0f || disabled.saturation != FeedbackSaturation::none)
        return 1;
    settings.enabled = true;
    settings.strength = 35;

    const auto settleOutput = [&](const FeedbackSample& sample) {
        feedback.reset();
        FeedbackOutput value{};
        for (int i = 0; i < 30; ++i) {
            value = feedback.update(settings, sample, 0.02f);
        }
        return value;
    };
    const auto settle = [&](const FeedbackSample& sample) { return settleOutput(sample).force; };
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
    const float firstSkidForce = feedback.update(settings, turn, 0.02f).force;
    const float skidForce = settle(turn);
    if (!(turnForce < firstSkidForce && firstSkidForce < skidForce && skidForce < 0.0f))
        return 16;
    turn.skidding = false;
    const float firstRecovered = feedback.update(settings, turn, 0.02f).force;
    for (int i = 0; i < 15; ++i)
        feedback.update(settings, turn, 0.02f);
    const float laterRecovered = feedback.update(settings, turn, 0.02f).force;
    if (!(turnForce < laterRecovered && laterRecovered < firstRecovered && firstRecovered < skidForce) ||
        !(skidForce - firstRecovered < firstSkidForce - turnForce))
        return 30;

    // Lateral motion alone gives a graded reduction. The game skid flag adds to that reduction.
    auto drifting = turn;
    drifting.sideSpeed = 5.0f;
    const float mildSlipForce = settle(drifting);
    drifting.sideSpeed = 14.0f;
    const float heavySlipForce = settle(drifting);
    drifting.skidding = true;
    const float heavySkidForce = settle(drifting);
    if (!(turnForce < mildSlipForce && mildSlipForce < heavySlipForce && heavySlipForce < heavySkidForce &&
          heavySkidForce < 0.0f))
        return 31;

    auto invalidTurn = turn;
    invalidTurn.sideSpeed = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = feedback.update(settings, invalidTurn, 0.02f);
    const float freshForce = WheelFeedback{}.update(settings, turn, 0.02f).force;
    if (invalid.force != 0.0f || invalid.requestedForce != 0.0f || invalid.saturation != FeedbackSaturation::none ||
        feedback.update(settings, turn, 0.02f).force != freshForce)
        return 32;

    settle(turn);
    turn.airborne = true;
    const float firstAirborne = feedback.update(settings, turn, 0.02f).force;
    const float airborneForce = settle(turn);
    if (!(turnForce < firstAirborne && firstAirborne < airborneForce && airborneForce < 0.0f))
        return 17;
    settings.centering = 150;
    const auto stronger = settleOutput(straight);
    if (std::abs(stronger.force - 1.5f * straightForce) > 0.0001f || stronger.saturation != FeedbackSaturation::none)
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
    settings.impactStrength = 150;
    if (std::abs(settle(motion) - 1.5f * impact) > 0.0001f || settle(straight) != straightForce)
        return 26;
    settings.impactStrength = 0;
    if (settle(motion) != 0.0f || settle(straight) != straightForce)
        return 27;
    settings.impactStrength = 100;
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
    settings.impactStrength = 150;
    if (std::abs(settle(landing) - 1.5f * landingForce) > 0.0001f)
        return 28;
    settings.impactStrength = 0;
    if (settle(landing) != 0.0f)
        return 29;
    settings.impactStrength = 100;
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
    if (feedback.update(settings, motion, 0.02f).force != 0.0f)
        return 8;

    // Centering and impact can add up to more than the nominal force.
    const FeedbackSample hardImpact{-1.0f, 120.0f, 0.0f, 3000.0f, 22.0f, 1.0f};
    auto strongCollision = hardImpact;
    strongCollision.steering = -0.85f;
    const auto impactPushedOverLimit = settleOutput(strongCollision);
    if (impactPushedOverLimit.force != -1.0f || impactPushedOverLimit.requestedForce >= -1.0f ||
        impactPushedOverLimit.saturation != FeedbackSaturation::transient)
        return 11;
    settings.strength = 200;
    settings.impactStrength = maxFeedbackImpactPercent;
    const auto both = settleOutput(hardImpact);
    if (both.force != -1.0f || both.requestedForce >= -1.0f || both.saturation != FeedbackSaturation::continuous)
        return 14;
    settings.invert = false;
    const auto reverse = settleOutput(hardImpact);
    if (reverse.force != 1.0f || reverse.requestedForce != -both.requestedForce ||
        reverse.saturation != both.saturation)
        return 12;
    auto opposingImpact = hardImpact;
    opposingImpact.impactSide = -1.0f;
    const auto cancelled = settleOutput(opposingImpact);
    if (cancelled.saturation != FeedbackSaturation::none || cancelled.requestedForce != cancelled.force)
        return 33;
    opposingImpact.steering = -0.75f;
    const auto clippedOpposite = settleOutput(opposingImpact);
    if (clippedOpposite.force != -1.0f || clippedOpposite.saturation != FeedbackSaturation::transient)
        return 34;
    FeedbackSample hardLanding{0.7f, 40.0f, 0.0f, 1700.0f};
    hardLanding.landingSpeed = 9.0f;
    hardLanding.landingAge = 0.135f;
    const auto landingPushedOverLimit = settleOutput(hardLanding);
    if (landingPushedOverLimit.force != -1.0f || landingPushedOverLimit.requestedForce >= -1.0f ||
        landingPushedOverLimit.saturation != FeedbackSaturation::transient)
        return 35;

    settings = {.enabled = true, .strength = 100};
    const FeedbackSample parking{0.5f, 0.0f, 0.0f, 1700.0f};
    const float standardParking = settle(parking);
    settings.lowSpeedEffort = 200;
    if (!(settle(parking) < standardParking - 0.045f))
        return 36;
    settings.lowSpeedEffort = 0;
    if (settle(parking) != 0.0f)
        return 37;
    settings.lowSpeedEffort = 100;
    const auto forceInMotion = [&](int damping, float speed) {
        settings.damping = damping;
        feedback.reset();
        auto sample = parking;
        sample.steering = 0.0f;
        sample.forwardSpeed = speed;
        feedback.update(settings, sample, 0.020f);
        sample.steering = 0.1f;
        return feedback.update(settings, sample, 0.020f).continuousForce;
    };
    if (!(forceInMotion(200, 10.0f) < forceInMotion(0, 10.0f) - 0.02f))
        return 38;

    const auto path = std::filesystem::current_path()
        / (L"feedback-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::string error;
    const auto defaults = loadFeedbackSettings(path, error);
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=35\nimpacts=0\ninvert=1\n";
    }
    const bool legacy = loadFeedbackSettings(path, error) == FeedbackSettings{true, 35, false, true};
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=35\nimpacts=0\ninvert=1\ncentering=165\n";
    }
    const bool previous = loadFeedbackSettings(path, error) == FeedbackSettings{true, 35, false, true, 165};
    settings.centering = 165;
    settings.impactStrength = 135;
    settings.lowSpeedEffort = 180;
    settings.damping = 45;
    const bool roundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessive{true, 201, true, false};
    const bool rejectedSave = !saveFeedbackSettings(path, excessive, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessiveCentering{true, 200, true, false, 251};
    const bool rejectedCentering = !saveFeedbackSettings(path, excessiveCentering, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    const FeedbackSettings excessiveImpact{true, 200, true, false, 100, maxFeedbackImpactPercent + 1};
    const bool rejectedImpact = !saveFeedbackSettings(path, excessiveImpact, error) && !error.empty() &&
        loadFeedbackSettings(path, error) == settings;
    settings.impactStrength = 0;
    const bool muteRoundTrip = saveFeedbackSettings(path, settings, error) && loadFeedbackSettings(path, error) == settings;
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\ncentering=49\n";
    }
    const bool rejected = !loadFeedbackSettings(path, error) && !error.empty();
    {
        std::ofstream file(path, std::ios::trunc);
        file << "version=1\nenabled=1\nstrength=100\nimpacts=1\ninvert=0\nimpact_strength=-1\n";
    }
    const bool rejectedNegativeImpact = !loadFeedbackSettings(path, error) && !error.empty();
    std::filesystem::remove(path);
    return defaults == FeedbackSettings{} && legacy && previous && roundTrip && muteRoundTrip && rejectedSave &&
        rejectedCentering && rejectedImpact && rejected && rejectedNegativeImpact ? 0 : 9;
}
