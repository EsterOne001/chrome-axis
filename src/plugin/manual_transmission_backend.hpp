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
    const char* outcome{"automatic"};
    bool manualActive{};
    std::optional<int> selectedGear;
    bool requestPending{};
    std::uint64_t requestedAt{};
    std::optional<std::uint64_t> engagementDelayMs; // From the request to the first settled observation.
    std::uint64_t telemetrySampledAt{};
    std::optional<float> engineRpm;
    std::optional<float> speedMetersPerSecond;
};

bool attach(RED4ext::v1::PluginHandle plugin, const RED4ext::v1::Sdk* sdk) noexcept;
void detach() noexcept;
bool available() noexcept;
// The absolute value is a selector level; delta is a single sequential input.
void publish(void* vehicle, bool enabled, int delta, std::optional<int> absolute = std::nullopt,
             bool captureTelemetry = false) noexcept;
Snapshot snapshot(void* vehicle = nullptr) noexcept;

namespace detail {

constexpr int neutralGear = -1;
constexpr int reverseGear = 0;
constexpr int unavailableGear = -2;

enum class Direction { unknown, forward, reverse };
enum class ShiftPlan { unsupported, alreadyEngaged, direct, stoppedDirect, stoppedThroughNeutral };
enum class Engagement { pending, confirmed, unconfirmed };

constexpr Engagement observeEngagement(int requested, int engaged, bool settled, std::uint64_t now,
                                        std::uint64_t expiresAt) noexcept {
    if (settled && engaged == requested)
        return Engagement::confirmed;
    return now < expiresAt ? Engagement::pending : Engagement::unconfirmed;
}

constexpr bool supportedGear(int gear, int entries) noexcept {
    // The native table contains up to eight entries, including reverse at index zero.
    return entries >= 2 && entries <= 8 && gear >= neutralGear && gear < entries;
}

constexpr bool forwardGear(int gear, int entries) noexcept {
    return gear > reverseGear && supportedGear(gear, entries);
}

constexpr int sequentialDestination(int engaged, int delta, int entries) noexcept {
    if (forwardGear(engaged, entries))
        return engaged + delta;
    // From neutral, a single upshift can select first. planShift still checks direction.
    return engaged == neutralGear && delta == 1 ? 1 : unavailableGear;
}

constexpr Direction observeDirection(Direction previous, int engaged, int entries) noexcept {
    if (!supportedGear(engaged, entries))
        return Direction::unknown;
    if (engaged == neutralGear)
        return previous;
    return engaged == reverseGear ? Direction::reverse : Direction::forward;
}

constexpr ShiftPlan planShift(int engaged, int requested, int entries, Direction previous) noexcept {
    if (!supportedGear(engaged, entries) || !supportedGear(requested, entries))
        return ShiftPlan::unsupported;
    if (requested == engaged)
        return ShiftPlan::alreadyEngaged;
    if (requested == neutralGear)
        return ShiftPlan::direct;
    if (engaged == reverseGear || (requested == reverseGear && engaged != neutralGear))
        return ShiftPlan::stoppedThroughNeutral;
    if (requested == reverseGear || (engaged == neutralGear && previous != Direction::forward))
        return ShiftPlan::stoppedDirect;
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
