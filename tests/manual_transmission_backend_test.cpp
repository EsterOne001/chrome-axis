#include "../src/plugin/manual_transmission_backend.hpp"
#include "../src/plugin/manual_transmission_input.hpp"

#include <limits>

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

constexpr manual_backend::Snapshot gearbox{
    .engagedGear = 1, .targetGear = 1, .gearCount = eightEntries, .sampledAt = 500,
};
static_assert(!freshGearbox({}, 500));
static_assert(!freshGearbox(gearbox, 499));
static_assert(freshGearbox(gearbox, 600));
static_assert(!freshGearbox(gearbox, 601));
static_assert(!requestExpired(0, 1000));
static_assert(!requestExpired(900, 899));
static_assert(requestExpired(900, 900));

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

constexpr bool canPlanShiftAfterPause(int gearBeforeNeutral, int requested, ShiftPlan expected) {
    int vehicle{};
    DirectionHistory history;
    history.setVehicle(&vehicle);
    history.observe(&vehicle, gearBeforeNeutral, gearBeforeNeutral, eightEntries);
    history.observe(&vehicle, neutralGear, neutralGear, eightEntries);

    // The input lease and gearbox observation expire, but the driver is still mounted.
    history.setVehicle(&vehicle);
    const auto direction = history.observe(&vehicle, neutralGear, neutralGear, eightEntries);
    return planShift(neutralGear, requested, eightEntries, direction) == expected;
}

static_assert(canPlanShiftAfterPause(3, 2, ShiftPlan::direct));
static_assert(canPlanShiftAfterPause(3, reverseGear, ShiftPlan::stoppedDirect));
static_assert(canPlanShiftAfterPause(reverseGear, 2, ShiftPlan::stoppedDirect));

constexpr bool clearsDirectionWhenDriverLeaves() {
    int oldVehicle{}, otherVehicle{};
    DirectionHistory history;
    history.setVehicle(&oldVehicle);
    history.observe(&oldVehicle, 3, 3, eightEntries);
    history.setVehicle(&otherVehicle);
    if (history.observe(&otherVehicle, neutralGear, neutralGear, eightEntries) != Direction::unknown)
        return false;

    history.observe(&otherVehicle, 2, 2, eightEntries);
    history.setVehicle(nullptr);
    history.setVehicle(&otherVehicle);
    return history.observe(&otherVehicle, neutralGear, neutralGear, eightEntries) == Direction::unknown;
}

static_assert(clearsDirectionWhenDriverLeaves());

constexpr bool requiresStopWhenReverseMayHaveEngagedDuringPause() {
    int vehicle{};
    DirectionHistory history;
    history.setVehicle(&vehicle);
    history.observe(&vehicle, 3, 3, eightEntries);
    history.reverseRequested(&vehicle);
    const auto afterReverseRequest = history.observe(&vehicle, neutralGear, neutralGear, eightEntries);
    history.observe(&vehicle, 3, 3, eightEntries);
    const auto afterReverseTarget = history.observe(&vehicle, neutralGear, reverseGear, eightEntries);
    return planShift(neutralGear, 2, eightEntries, afterReverseRequest) == ShiftPlan::stoppedDirect
        && planShift(neutralGear, 2, eightEntries, afterReverseTarget) == ShiftPlan::stoppedDirect;
}

static_assert(requiresStopWhenReverseMayHaveEngagedDuringPause());

constexpr bool forwardRatiosAfterMenu() {
    for (const auto prior : {Direction::unknown, Direction::forward, Direction::reverse}) {
        for (int gear = 1; gear < eightEntries; ++gear) {
            if (planShift(neutralGear, gear, eightEntries, prior, Direction::forward) != ShiftPlan::direct)
                return false;
        }
    }
    return true;
}

static_assert(forwardRatiosAfterMenu());
static_assert(planShift(neutralGear, 7, 6, Direction::unknown, Direction::forward) == ShiftPlan::unsupported);
static_assert(planShift(neutralGear, 3, eightEntries, Direction::forward, Direction::reverse) == ShiftPlan::stoppedDirect);
static_assert(planShift(neutralGear, reverseGear, eightEntries, Direction::unknown, Direction::forward) == ShiftPlan::stoppedDirect);
static_assert(planShift(reverseGear, 3, eightEntries, Direction::reverse, Direction::forward) == ShiftPlan::stoppedThroughNeutral);
static_assert(shiftSpeedAllowed(reverseGear, 2.5f / 3.6f));
static_assert(shiftSpeedAllowed(reverseGear, static_cast<float>(3.0 / 3.6)));
static_assert(!shiftSpeedAllowed(reverseGear, 3.01f / 3.6f));
static_assert(!shiftSpeedAllowed(reverseGear, std::nullopt));
static_assert(shiftSpeedAllowed(1, 0.5f));
static_assert(!shiftSpeedAllowed(1, 2.5f / 3.6f));

constexpr bool checksHeadingTimeAndVehicle() {
    int vehicle{}, otherVehicle{};
    const VehicleHeading heading{&vehicle, {0.0f, 1.0f, 0.0f}, 100};
    return heading.current(&vehicle, 100) && heading.current(&vehicle, 300)
        && !heading.current(&vehicle, 301) && !heading.current(&vehicle, 99)
        && !heading.current(&otherVehicle, 100) && !VehicleHeading{}.current(&vehicle, 100);
}

static_assert(checksHeadingTimeAndVehicle());

static_assert(suppressAutomatic(2, 3, eightEntries, false, std::nullopt));
static_assert(!suppressAutomatic(2, 3, eightEntries, true, std::nullopt));
static_assert(!suppressAutomatic(2, neutralGear, eightEntries, false, std::nullopt));
static_assert(suppressAutomatic(neutralGear, 1, eightEntries, false, neutralGear));
static_assert(suppressAutomatic(neutralGear, reverseGear, eightEntries, true, neutralGear));
static_assert(!suppressAutomatic(1, neutralGear, eightEntries, false, neutralGear));
static_assert(suppressAutomatic(reverseGear, 1, eightEntries, true, reverseGear));
static_assert(!suppressAutomatic(reverseGear, 1, eightEntries, true, 8));
} // namespace

int main() {
    constexpr std::array forward{0.0f, 1.0f, 0.0f};
    constexpr std::array uphillForward{0.0f, 0.8f, 0.6f};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const int engaged : {reverseGear, 3}) {
        const auto priorDirection = observeDirection(Direction::unknown, engaged, eightEntries);
        for (const auto speed : {0.0f, 6.0f}) {
            chrome_axis::TransmissionPedals pedals;
            using chrome_axis::observePedalGear;
            using chrome_axis::PedalRouting;
            const auto inGear = engaged == reverseGear ? PedalRouting::reverse : PedalRouting::standard;
            if (planShift(engaged, neutralGear, eightEntries, priorDirection) != ShiftPlan::direct ||
                pedals.update(true, observePedalGear(engaged, engaged, eightEntries), {}).routing != inGear ||
                pedals.update(true, observePedalGear(engaged, neutralGear, eightEntries), {}).routing != inGear ||
                pedals.update(true, observePedalGear(neutralGear, neutralGear, eightEntries), {})
                    .routing != PedalRouting::standard)
                return 2;
            const auto movement = speed == 0.0f ? Direction::unknown : priorDirection;
            const auto next = planShift(neutralGear, 1, eightEntries, priorDirection, movement);
            const bool forwardAllowed = next == ShiftPlan::direct ||
                (next == ShiftPlan::stoppedDirect && shiftSpeedAllowed(1, speed));
            if (forwardAllowed != (engaged != reverseGear || speed == 0.0f))
                return 3;
        }
    }
    return movementDirection({0.0f, 10.0f, 0.0f}, forward) == Direction::forward
        && movementDirection({0.0f, 8.0f, 6.0f}, uphillForward) == Direction::forward
        && movementDirection({0.0f, -10.0f, 0.0f}, forward) == Direction::reverse
        && movementDirection({10.0f, 0.6f, 0.0f}, forward) == Direction::unknown
        && movementDirection({0.0f, 0.5f, 0.0f}, forward) == Direction::unknown
        && movementDirection({0.0f, 10.0f, 0.0f}, {}) == Direction::unknown
        && movementDirection({0.0f, 10.0f, 0.0f}, {0.0f, nan, 0.0f}) == Direction::unknown
        && movementDirection({0.0f, nan, 0.0f}, forward) == Direction::unknown ? 0 : 1;
}
