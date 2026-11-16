#pragma once

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

#include <array>
#include <cmath>
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
    bool manualRequested{};
    bool reverseIssued{};
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
void setDriverVehicle(const void* vehicle, std::optional<std::array<float, 3>> forward = std::nullopt) noexcept;
// The absolute value is a selector level; delta is a single sequential input.
void publish(void* vehicle, bool enabled, int delta, std::optional<int> absolute = std::nullopt,
             bool captureTelemetry = false) noexcept;
Snapshot snapshot(void* vehicle = nullptr) noexcept;

namespace detail {

constexpr int neutralGear = -1;
constexpr int reverseGear = 0;
constexpr int unavailableGear = -2;
constexpr std::uint64_t gearboxObservationLifetimeMs = 100;
constexpr std::uint64_t headingLifetimeMs = 200;
constexpr float directionChangeSpeed = 0.5f; // m/s

enum class Direction { unknown, forward, reverse };
enum class ShiftPlan { unsupported, alreadyEngaged, direct, stoppedDirect, stoppedThroughNeutral };
enum class Engagement { pending, confirmed, unconfirmed };

struct VehicleHeading {
    const void* vehicle{};
    std::array<float, 3> forward{};
    std::uint64_t sampledAt{};

    constexpr std::optional<std::array<float, 3>> current(const void* owner, std::uint64_t now) const noexcept {
        if (!owner || vehicle != owner || !sampledAt || now < sampledAt || now - sampledAt > headingLifetimeMs)
            return std::nullopt;
        return forward;
    }
};

inline Direction movementDirection(const std::array<float, 3>& velocity,
                                   const std::array<float, 3>& forward) noexcept {
    const float alignment = forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    if (!std::isfinite(alignment) || alignment < 0.9f || alignment > 1.1f)
        return Direction::unknown;
    const float longitudinal = velocity[0] * forward[0] + velocity[1] * forward[1] + velocity[2] * forward[2];
    if (!std::isfinite(longitudinal))
        return Direction::unknown;
    if (longitudinal < -directionChangeSpeed)
        return Direction::reverse;
    if (longitudinal <= directionChangeSpeed)
        return Direction::unknown;

    // Sideways slides alone cannot establish that the car is rolling forwards.
    const float speed = std::hypot(velocity[0], velocity[1], velocity[2]);
    return longitudinal >= speed * 0.5f ? Direction::forward : Direction::unknown;
}

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

constexpr bool freshGearbox(const Snapshot& observation, std::uint64_t now) noexcept {
    return observation.sampledAt && now >= observation.sampledAt &&
           now - observation.sampledAt <= gearboxObservationLifetimeMs &&
           supportedGear(observation.engagedGear, observation.gearCount) &&
           supportedGear(observation.targetGear, observation.gearCount);
}

constexpr bool requestExpired(std::uint64_t deadline, std::uint64_t now) noexcept {
    return deadline && now >= deadline;
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

class DirectionHistory {
public:
    constexpr void setVehicle(const void* vehicle) noexcept {
        if (vehicle_ != vehicle) {
            vehicle_ = vehicle;
            direction_ = Direction::unknown;
        }
    }

    constexpr Direction observe(const void* vehicle, int engaged, int target, int entries) noexcept {
        if (!vehicle || vehicle_ != vehicle)
            return Direction::unknown;
        if (!supportedGear(target, entries) || (target == reverseGear && engaged != reverseGear))
            direction_ = Direction::unknown;
        else
            direction_ = observeDirection(direction_, engaged, entries);
        return direction_;
    }

    constexpr void reverseRequested(const void* vehicle) noexcept {
        if (vehicle_ == vehicle)
            direction_ = Direction::unknown;
    }

private:
    const void* vehicle_{};
    Direction direction_{Direction::unknown};
};

constexpr ShiftPlan planShift(int engaged, int requested, int entries, Direction previous,
                              Direction movement = Direction::unknown) noexcept {
    if (!supportedGear(engaged, entries) || !supportedGear(requested, entries))
        return ShiftPlan::unsupported;
    if (requested == engaged)
        return ShiftPlan::alreadyEngaged;
    if (requested == neutralGear)
        return ShiftPlan::direct;
    if (engaged == reverseGear || (requested == reverseGear && engaged != neutralGear))
        return ShiftPlan::stoppedThroughNeutral;
    const auto direction = movement == Direction::unknown ? previous : movement;
    if (requested == reverseGear || (engaged == neutralGear && direction != Direction::forward))
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
