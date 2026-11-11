#pragma once

#include "chrome_axis/controls.hpp"

namespace chrome_axis {

struct AxisCapture {
    std::array<long, 8> median{};
    std::array<double, 8> spread{};
    std::array<bool, 8> axesPresent{};
};

struct AxisCandidate {
    std::size_t axis{};
    double travel{};
    float deadzone{};
};

class CalibrationCapture {
public:
    static constexpr std::size_t sampleCount = 15;

    bool add(const DeviceSample& sample);
    bool complete() const noexcept;
    std::optional<AxisCapture> result() const;

private:
    std::array<std::array<long, sampleCount>, 8> readings_{};
    std::array<bool, 8> axesPresent_{};
    std::size_t count_{};
};

std::vector<AxisCandidate> axisCandidates(const AxisCapture& neutral, const AxisCapture& negative,
                                          const AxisCapture& positive, bool steering);
AxisCalibration makeCalibration(std::string deviceId, const AxisCapture& neutral,
                                const AxisCapture& negative, const AxisCapture& positive,
                                const AxisCandidate& candidate);

} // namespace chrome_axis
