#pragma once

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

#include <cstdint>

namespace manual_backend {

struct Snapshot {
    int engagedGear{-1};
    int targetGear{-1};
    int requestedGear{-1};
    int gearCount{};
    std::uint64_t sampledAt{};
    float speedMetersPerSecond{-1.0f};
    float engineRpm{-1.0f};
    unsigned shiftSerial{};
    unsigned suppressedAutoShifts{};
    const char* outcome{"automatic"};
};

bool attach(RED4ext::v1::PluginHandle plugin, const RED4ext::v1::Sdk* sdk) noexcept;
void detach() noexcept;
void publish(void* vehicle, bool enabled, int delta) noexcept;
Snapshot snapshot() noexcept;

} // namespace manual_backend
