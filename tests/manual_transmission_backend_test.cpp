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

static_assert(planShift(2, neutralGear, eightEntries, false) == ShiftPlan::direct);
static_assert(planShift(neutralGear, 2, eightEntries, false) == ShiftPlan::direct);
static_assert(planShift(3, 2, eightEntries, false) == ShiftPlan::direct);
static_assert(planShift(2, reverseGear, eightEntries, false) == ShiftPlan::requiresStop);
static_assert(planShift(neutralGear, reverseGear, eightEntries, false) == ShiftPlan::requiresStop);
static_assert(planShift(reverseGear, 1, eightEntries, false) == ShiftPlan::requiresStop);
static_assert(planShift(2, reverseGear, eightEntries, true) == ShiftPlan::throughNeutral);
static_assert(planShift(reverseGear, 1, eightEntries, true) == ShiftPlan::throughNeutral);
static_assert(planShift(neutralGear, reverseGear, eightEntries, true) == ShiftPlan::direct);
static_assert(planShift(reverseGear, reverseGear, eightEntries, false) == ShiftPlan::alreadyEngaged);
static_assert(planShift(unavailableGear, reverseGear, eightEntries, true) == ShiftPlan::unsupported);

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
