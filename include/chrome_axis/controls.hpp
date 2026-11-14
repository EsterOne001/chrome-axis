#pragma once

#include "chrome_axis/input.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace chrome_axis {

inline constexpr float minimumPedalDeadzone = 0.005f;
inline constexpr float maximumAxisDeadzone = 0.25f;

struct AxisCalibration {
    std::string deviceId;
    std::size_t axis{};
    long neutral{};
    long negative{};
    long positive{};
    float deadzone{};
};

inline constexpr int minimumBrakeProgressivity = -100;
inline constexpr int maximumBrakeProgressivity = 100;
inline constexpr int minimumBrakeSaturation = 50;
inline constexpr int maximumBrakeSaturation = 100;

struct BrakeResponse {
    int progressivity{};
    int saturation{maximumBrakeSaturation};

    bool operator==(const BrakeResponse&) const = default;
};

struct BrakePreview {
    float raw{};
    float adjusted{};
};

bool validateBrakeResponse(const BrakeResponse& response) noexcept;
// A valid calibration and response produce fractions of calibrated travel and brake output.
BrakePreview previewBrake(const AxisCalibration& calibration, long raw, const BrakeResponse& response) noexcept;

struct Profile {
    AxisCalibration steering;
    AxisCalibration throttle;
    AxisCalibration brake;
    BrakeResponse brakeResponse;
};

struct Controls {
    float steering{};
    float throttle{};
    float brake{};
};

struct ControlState {
    Controls values{};
    bool steeringReady{};
    bool throttleReady{};
    bool brakeReady{};
};

enum class AxisStatus {
    unassigned,
    deviceUnavailable,
    axisUnavailable,
    waitingForNeutral,
    armed,
};

inline constexpr wchar_t profileFilename[] = L"chrome_axis.ini";

// The pedals can share one centered axis if they move it in opposite directions.
bool compatibleAxisPair(const AxisCalibration& throttle, const AxisCalibration& brake) noexcept;
bool validateProfile(const Profile& profile, std::string& error);
std::optional<Profile> loadProfile(const std::filesystem::path& path, std::string& error);
bool saveProfile(const std::filesystem::path& path, const Profile& profile, std::string& error);

class ControlReader {
public:
    using Readings = std::array<std::optional<float>, 3>;

    explicit ControlReader(const Profile& profile);
    ControlState poll(DirectInput& input, Readings* readings = nullptr);
    void reset() noexcept;

    // Status from the latest sample in steering, throttle, and brake order.
    const std::array<AxisStatus, 3>& axisStatuses() const noexcept { return axisStatuses_; }

    // Each channel refers to one sample. Channels on the same device share a sample.
    // Requested diagnostic readings are independent of the neutral gate.
    ControlState update(const std::array<const DeviceSample*, 3>& samples, Readings* readings = nullptr);

private:
    Profile profile_;
    bool combinedPedals_{};
    std::array<std::size_t, 3> sampleOwners_{};
    std::array<unsigned int, 3> neutralPolls_{};
    std::array<AxisStatus, 3> axisStatuses_{};
};

} // namespace chrome_axis
