#pragma once

#include "chrome_axis/calibration.hpp"
#include "chrome_axis/shifter.hpp"

#include <algorithm>
#include <utility>

namespace chrome_axis {

inline bool shifterRestStillNeutral(const AccessoryBinding& binding, const AxisCapture& rest) noexcept
{
    if (binding.kind != AccessoryKind::Axis || binding.index >= axisCount)
        return false;
    DeviceSample sample;
    sample.axes = rest.median;
    sample.axesPresent = rest.axesPresent;
    return discreteAccessoryLevel(binding, sample, false) == DiscreteAccessoryLevel::neutral;
}

struct ShifterSetup {
    std::string_view deviceId;
    std::size_t forward{};
    bool native{};

    bool replaces(const AccessoryBinding& binding) const noexcept
    {
        if (!native)
            return forward != 0 && binding.nativeShifter && binding.deviceId == deviceId;

        // A profile can have one native transmission source.
        if (binding.nativeShifter || binding.nativeSequential)
            return true;
        return forward != 0 && binding.deviceId == deviceId && binding.key != 0 &&
               nativeShifterPositionBit(binding.role) != 0;
    }

    std::optional<AccessoryProfile> withCapture(const AccessoryProfile& saved, const AccessoryProfile& draft,
                                               unsigned int absentPositions) const
    {
        unsigned int presentPositions = 0;
        if (native && forward != 0) {
            for (const auto& binding : draft)
                presentPositions |= nativeShifterPositionBit(binding.role);
            if (forward > 8)
                return std::nullopt;
            const unsigned int expected = (1u << (forward + 1)) - 1u;
            if ((presentPositions | absentPositions) != expected || (presentPositions & absentPositions) != 0)
                return std::nullopt;
        } else if (native &&
                   (draft.size() != 2 || draft[0].role != ShifterRole::Upshift ||
                    draft[1].role != ShifterRole::Downshift)) {
            return std::nullopt;
        }

        auto updated = saved;
        std::erase_if(updated, [this](const AccessoryBinding& binding) { return replaces(binding); });
        for (auto binding : draft) {
            if (binding.nativeShifter)
                binding.nativeShifterPositions = presentPositions;
            const auto existing = binding.nativeShifter || binding.nativeSequential ? updated.end()
                : std::ranges::find(updated, binding.key, &AccessoryBinding::key);
            if (existing == updated.end()) {
                updated.push_back(std::move(binding));
            } else {
                binding.thresholds = existing->thresholds;
                *existing = std::move(binding);
            }
        }
        return updated;
    }
};

} // namespace chrome_axis
