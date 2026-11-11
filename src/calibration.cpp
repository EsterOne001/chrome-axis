#include "chrome_axis/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace chrome_axis {

bool CalibrationCapture::add(const DeviceSample& sample)
{
    if (complete())
        return false;
    if (count_ == 0) {
        if (std::ranges::none_of(sample.axesPresent, [](bool present) { return present; }))
            return false;
        axesPresent_ = sample.axesPresent;
    } else if (axesPresent_ != sample.axesPresent) {
        return false;
    }
    for (std::size_t axis = 0; axis < readings_.size(); ++axis)
        readings_[axis][count_] = sample.axes[axis];
    ++count_;
    return true;
}

bool CalibrationCapture::complete() const noexcept
{
    return count_ == sampleCount;
}

std::optional<AxisCapture> CalibrationCapture::result() const
{
    if (!complete())
        return std::nullopt;

    AxisCapture capture;
    capture.axesPresent = axesPresent_;
    for (std::size_t i = 0; i < readings_.size(); ++i) {
        if (!axesPresent_[i])
            continue;
        auto values = readings_[i];
        std::sort(values.begin(), values.end());
        capture.median[i] = values[sampleCount / 2];
        // Isolated driver spikes should not decide whether the held position was stable.
        capture.spread[i] = static_cast<double>(values[values.size() - 2]) - values[1];
    }
    return capture;
}

std::vector<AxisCandidate> axisCandidates(const AxisCapture& neutral, const AxisCapture& negative,
                                          const AxisCapture& positive, bool steering)
{
    std::vector<AxisCandidate> candidates;
    for (std::size_t i = 0; i < neutral.axesPresent.size(); ++i) {
        if (!neutral.axesPresent[i] || !positive.axesPresent[i] || (steering && !negative.axesPresent[i]))
            continue;

        const double full = static_cast<double>(positive.median[i]) - neutral.median[i];
        const double opposite = static_cast<double>(negative.median[i]) - neutral.median[i];
        if (steering && full * opposite >= 0.0)
            continue;

        const double travel = steering ? std::min(std::abs(full), std::abs(opposite)) : std::abs(full);
        const double spread = steering
            ? std::max({neutral.spread[i], negative.spread[i], positive.spread[i]})
            : std::max(neutral.spread[i], positive.spread[i]);
        if (travel < 16.0 || travel < 8.0 * spread)
            continue;

        const auto deadzone = static_cast<float>(std::clamp(
            2.0 * neutral.spread[i] / travel, steering ? 0.02 : 0.03, 0.20));
        candidates.push_back({i, travel, deadzone});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const AxisCandidate& a, const AxisCandidate& b) {
        return a.travel > b.travel;
    });
    return candidates;
}

AxisCalibration makeCalibration(std::string deviceId, const AxisCapture& neutral,
                                const AxisCapture& negative, const AxisCapture& positive,
                                const AxisCandidate& candidate)
{
    const auto axis = candidate.axis;
    return {std::move(deviceId), axis, neutral.median[axis], negative.median[axis], positive.median[axis],
            candidate.deadzone};
}

} // namespace chrome_axis
