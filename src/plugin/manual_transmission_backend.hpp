#pragma once

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

#include <cstdint>
#include <optional>

namespace manual_backend {

struct Snapshot {
    int engagedGear{-1};
    int targetGear{-1};
    int requestedGear{-2};
    int gearCount{};
    std::uint64_t sampledAt{};
    float speedMetersPerSecond{-1.0f};
    float engineRpm{-1.0f};
    unsigned shiftSerial{};
    unsigned suppressedAutoShifts{};
    const char* outcome{"automatic"};
    bool manualActive{};
    std::optional<int> selectedGear;
};

bool attach(RED4ext::v1::PluginHandle plugin, const RED4ext::v1::Sdk* sdk) noexcept;
void detach() noexcept;
bool available() noexcept;
// The absolute value is a selector level; delta is a single sequential input.
void publish(void* vehicle, bool enabled, int delta, std::optional<int> absolute = std::nullopt) noexcept;
Snapshot snapshot(void* vehicle = nullptr) noexcept;

namespace detail {

constexpr int neutralGear = -1;
constexpr int reverseGear = 0;
constexpr int unavailableGear = -2;

enum class ShiftPlan { unsupported, alreadyEngaged, requiresStop, throughNeutral, direct };

constexpr bool supportedGear(int gear, int entries) noexcept {
    // The native table contains up to eight entries, including reverse at index zero.
    return entries >= 2 && entries <= 8 && gear >= neutralGear && gear < entries;
}

constexpr bool forwardGear(int gear, int entries) noexcept {
    return gear > reverseGear && supportedGear(gear, entries);
}

constexpr ShiftPlan planShift(int engaged, int requested, int entries, bool nearStop) noexcept {
    if (!supportedGear(engaged, entries) || !supportedGear(requested, entries))
        return ShiftPlan::unsupported;
    if (requested == engaged)
        return ShiftPlan::alreadyEngaged;
    const bool fromReverse = engaged == reverseGear && requested > reverseGear;
    if ((requested == reverseGear || fromReverse) && !nearStop)
        return ShiftPlan::requiresStop;
    if (fromReverse || (requested == reverseGear && engaged > reverseGear))
        return ShiftPlan::throughNeutral;
    return ShiftPlan::direct;
}

constexpr bool suppressAutomatic(int engaged, int target, int entries, bool bypassCooldown,
                                 std::optional<int> heldGear) noexcept {
    if (engaged == target || !supportedGear(engaged, entries) || !supportedGear(target, entries))
        return false;
    if (heldGear && supportedGear(*heldGear, entries))
        return target != *heldGear;
    return !bypassCooldown && forwardGear(engaged, entries) && forwardGear(target, entries);
}

} // namespace detail

} // namespace manual_backend
