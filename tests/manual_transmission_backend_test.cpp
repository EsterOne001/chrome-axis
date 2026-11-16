#include "../src/plugin/manual_transmission_backend.hpp"

namespace {
using namespace manual_backend::detail;

constexpr int eightEntries = 8;
static_assert(supportedGear(neutralGear, eightEntries));
static_assert(supportedGear(reverseGear, eightEntries));
static_assert(forwardGear(7, eightEntries));
static_assert(!supportedGear(8, eightEntries));
static_assert(!supportedGear(4, 4));
static_assert(!supportedGear(unavailableGear, eightEntries));
static_assert(!supportedGear(reverseGear, 0));

static_assert(observeEngagement(3, neutralGear, false, 100, 200) == Engagement::pending);
static_assert(observeEngagement(3, 3, false, 180, 200) == Engagement::pending);
static_assert(observeEngagement(3, 3, true, 181, 200) == Engagement::confirmed);
static_assert(observeEngagement(3, 2, true, 200, 200) == Engagement::unconfirmed);
static_assert(observeEngagement(neutralGear, neutralGear, true, 180, 200) == Engagement::confirmed);

constexpr auto priorReverse = observeDirection(Direction::unknown, reverseGear, eightEntries);
constexpr auto neutralAfterReverse = observeDirection(priorReverse, neutralGear, eightEntries);
constexpr auto priorForward = observeDirection(Direction::unknown, 2, eightEntries);
constexpr auto neutralAfterForward = observeDirection(priorForward, neutralGear, eightEntries);
static_assert(neutralAfterReverse == Direction::reverse);
static_assert(neutralAfterForward == Direction::forward);
static_assert(observeDirection(priorForward, unavailableGear, eightEntries) == Direction::unknown);

// Neutral is available while coasting. A fresh forward selection or reselection must
// still wait for near-stop if the gearbox reached neutral from reverse.
static_assert(planShift(reverseGear, neutralGear, eightEntries, priorReverse) == ShiftPlan::direct);
static_assert(planShift(neutralGear, 1, eightEntries, neutralAfterReverse) == ShiftPlan::stoppedDirect);
static_assert(planShift(neutralGear, 3, eightEntries, neutralAfterReverse) == ShiftPlan::stoppedDirect);
static_assert(planShift(neutralGear, 1, eightEntries, Direction::unknown) == ShiftPlan::stoppedDirect);
static_assert(planShift(neutralGear, 2, eightEntries, neutralAfterForward) == ShiftPlan::direct);
static_assert(planShift(3, 2, eightEntries, priorForward) == ShiftPlan::direct);
static_assert(planShift(7, 3, eightEntries, priorForward) == ShiftPlan::direct);
static_assert(planShift(7, neutralGear, eightEntries, priorForward) == ShiftPlan::direct);
static_assert(sequentialDestination(neutralGear, 1, eightEntries) == 1);
static_assert(sequentialDestination(neutralGear, -1, eightEntries) == unavailableGear);
static_assert(sequentialDestination(reverseGear, 1, eightEntries) == unavailableGear);
static_assert(planShift(2, neutralGear, eightEntries, priorForward) == ShiftPlan::direct);
static_assert(planShift(2, reverseGear, eightEntries, priorForward) == ShiftPlan::stoppedThroughNeutral);
static_assert(planShift(neutralGear, reverseGear, eightEntries, neutralAfterForward) == ShiftPlan::stoppedDirect);
static_assert(planShift(neutralGear, reverseGear, eightEntries, neutralAfterReverse) == ShiftPlan::stoppedDirect);
static_assert(planShift(reverseGear, 1, eightEntries, priorReverse) == ShiftPlan::stoppedThroughNeutral);
static_assert(planShift(reverseGear, reverseGear, eightEntries, priorReverse) == ShiftPlan::alreadyEngaged);
static_assert(planShift(unavailableGear, reverseGear, eightEntries, Direction::unknown) == ShiftPlan::unsupported);

static_assert(suppressAutomatic(2, 3, eightEntries, false, std::nullopt));
static_assert(!suppressAutomatic(2, 3, eightEntries, true, std::nullopt));
static_assert(!suppressAutomatic(2, neutralGear, eightEntries, false, std::nullopt));
static_assert(suppressAutomatic(neutralGear, 1, eightEntries, false, neutralGear));
static_assert(suppressAutomatic(neutralGear, reverseGear, eightEntries, true, neutralGear));
static_assert(!suppressAutomatic(1, neutralGear, eightEntries, false, neutralGear));
static_assert(suppressAutomatic(reverseGear, 1, eightEntries, true, reverseGear));
static_assert(!suppressAutomatic(reverseGear, 1, eightEntries, true, 8));
} // namespace

int main() {}
