#pragma once

#include "chrome_axis/input.hpp"

#include <array>
#include <chrono>
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

inline constexpr int minimumResponseProgressivity = -100;
inline constexpr int maximumResponseProgressivity = 100;
inline constexpr int minimumResponseSaturation = 50;
inline constexpr int maximumResponseSaturation = 100;
inline constexpr int minimumBrakeProgressivity = minimumResponseProgressivity;
inline constexpr int maximumBrakeProgressivity = maximumResponseProgressivity;
inline constexpr int minimumBrakeSaturation = minimumResponseSaturation;
inline constexpr int maximumBrakeSaturation = maximumResponseSaturation;

enum class ResponseAxis { steering, throttle, brake };

struct AxisResponse {
    int progressivity{};
    int saturation{maximumResponseSaturation};
    bool stabilization{};

    bool operator==(const AxisResponse&) const = default;
};

using BrakeResponse = AxisResponse;

class PedalStabilizer {
public:
    using Clock = std::chrono::steady_clock;

    float update(float pedal, Clock::time_point sampledAt) noexcept;
    void reset() noexcept { previousSample_.reset(); }

private:
    std::optional<Clock::time_point> previousSample_;
    float input_{};
    float output_{};
    float logRetention_{};
};

struct AxisPreview {
    float raw{};
    float adjusted{};
};

using BrakePreview = AxisPreview;

bool validateAxisResponse(const AxisResponse& response) noexcept;
// Signed for steering. Saturation is defined on the calibrated travel before deadzone.
AxisPreview previewAxisResponse(ResponseAxis axis, const AxisCalibration& calibration, long raw,
                               const AxisResponse& response) noexcept;
bool validateBrakeResponse(const BrakeResponse& response) noexcept;
// A valid calibration and response produce fractions of calibrated travel and brake output.
BrakePreview previewBrake(const AxisCalibration& calibration, long raw, const BrakeResponse& response) noexcept;

struct Profile {
    AxisCalibration steering;
    AxisCalibration throttle;
    AxisCalibration brake;
    BrakeResponse brakeResponse;
    AxisResponse steeringResponse{};
    AxisResponse throttleResponse{};
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
    // The calibrated position after deadzone, before curve and useful-travel adjustments.
    float calibratedSteering{};
    std::array<DeviceReadClock::time_point, 3> sampledAt{};
    std::bitset<3> assigned{};
};

enum class AxisStatus {
    unassigned,
    deviceUnavailable,
    axisUnavailable,
    waitingForNeutral,
    armed,
    temporarilyUnavailable,
};

inline constexpr wchar_t profileFilename[] = L"chrome_axis.ini";

// The pedals can share one centered axis if they move it in opposite directions.
bool compatibleAxisPair(const AxisCalibration& throttle, const AxisCalibration& brake) noexcept;
bool validateProfile(const Profile& profile, std::string& error);
// A parse failure leaves readError set to ERROR_SUCCESS.
std::optional<Profile> loadProfile(const std::filesystem::path& path, std::string& error,
                                   DWORD* readError = nullptr);
bool saveProfile(const std::filesystem::path& path, const Profile& profile, std::string& error);

enum class PedalRestState : std::uint8_t { unavailable, atRest, outsideRest };

struct PedalRestReading {
    double position{}; // Signed calibrated position before the deadzone is applied.
    float threshold{};
    PedalRestState state{};
    bool symmetric{}; // Shared pedals use both sides of the rest position.
};

class ControlReader {
public:
    using Readings = std::array<std::optional<float>, 3>;
    struct CalibrationReadings {
        Readings travel;
        std::optional<float> adjustedBrake;
    };

    explicit ControlReader(const Profile& profile);
    ControlState poll(DeviceSampleSource& input, Readings* readings = nullptr);
    CalibrationReadings review(DeviceSampleSource& input);
    void reset() noexcept;

    // Status from the latest sample in steering, throttle, and brake order.
    const std::array<AxisStatus, 3>& axisStatuses() const noexcept { return axisStatuses_; }
    const std::array<PedalRestReading, 2>& pedalRestReadings() const noexcept { return pedalRestReadings_; }

    // Each channel refers to one sample. Channels on the same device share a sample.
    // Readings and calibration travel are available before the axis arms.
    ControlState update(const std::array<const DeviceSample*, 3>& samples, Readings* readings = nullptr,
        const std::array<DeviceReadStatus, 3>& statuses = {}, CalibrationReadings* calibration = nullptr,
        PedalStabilizer::Clock::time_point sampledAt = PedalStabilizer::Clock::now());

private:
    ControlState poll(DeviceSampleSource& input, Readings* readings, CalibrationReadings* calibration);

    Profile profile_;
    bool combinedPedals_{};
    std::array<std::size_t, 3> sampleOwners_{};
    std::array<unsigned int, 3> neutralPolls_{};
    std::array<std::uint64_t, 3> lastSequences_{};
    std::array<AxisStatus, 3> axisStatuses_{};
    std::array<PedalRestReading, 2> pedalRestReadings_{};
    std::array<PedalStabilizer, 2> pedalStabilizers_{};
};

} // namespace chrome_axis
