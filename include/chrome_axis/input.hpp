#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

inline constexpr std::size_t axisCount = 8;
inline constexpr std::size_t buttonCount = 128;
inline constexpr std::size_t povCount = 4;
inline constexpr std::uint16_t povCentered = 0xffff;
using DeviceReadClock = std::chrono::steady_clock;

struct DeviceObjectSelection {
    std::bitset<axisCount> axes;
    std::bitset<buttonCount> buttons;
    std::bitset<povCount> povs;

    bool operator==(const DeviceObjectSelection&) const = default;
};

struct DeviceInfo {
    std::string id;
    std::string name;
    std::array<bool, axisCount> axes{};
    std::bitset<buttonCount> buttons{};
    std::array<bool, povCount> povs{};
    std::array<bool, axisCount> forceFeedbackAxes{};
    std::string productName;
    DeviceObjectSelection layoutPending;

    bool operator==(const DeviceInfo&) const = default;
};

struct DeviceSample {
    std::array<long, axisCount> axes{};
    std::bitset<buttonCount> buttons{};
    std::array<bool, axisCount> axesPresent{};
    // Angles clockwise from up, in hundredths of a degree. Absent hats stay centered.
    std::array<std::uint16_t, povCount> povs{povCentered, povCentered, povCentered, povCentered};
    std::bitset<buttonCount> buttonsPresent{};
    std::array<bool, povCount> povsPresent{};
    std::bitset<axisCount> axesPending{};
    std::bitset<buttonCount> buttonsPending{};
    std::bitset<povCount> povsPending{};
    DeviceReadClock::time_point sampledAt{};
    std::uint64_t sequence{}; // Zero for synchronous reads.
    std::uint64_t recoveryToken{};
};

const char* axisName(std::size_t index) noexcept;
bool isDeviceId(std::string_view id) noexcept;

// An empty device ID refers to initialization. Availability is separate from HRESULT
// because DirectInput can report a detached device with a success-class HRESULT.
using DirectInputDiagnostic = void (*)(std::string_view deviceId, std::string_view stage,
    HRESULT result, bool available);

enum class DirectInputReadEventKind : std::uint8_t {
    slow, pending, unavailable, recovered, sampleExpired, discreteOverflow,
    layoutPending, layoutAcquireDenied, layoutMissing, layoutRecovered
};

struct DirectInputReadEvent {
    DirectInputReadEventKind kind{};
    std::string_view deviceId;
    std::string_view operation;
    HRESULT result{};
    std::uint64_t durationMicros{};
};

using DirectInputReadObserver = void (*)(void*, const DirectInputReadEvent&) noexcept;
using DirectInputActiveCall = std::atomic<const char*>; // Non-owning name with static storage.

enum class WheelForceState { waiting, active, unavailable, neutral };

struct WheelForceGuard {
    using Check = bool (*)(const void*) noexcept;
    Check current{};
    Check canInitialize{};
    const void* context{};
    // A nonzero token identifies an input connection confirmed after a physical interruption.
    std::uint64_t recoveryToken{};
};

// Only available and resumed carry fresh data. Resumed starts a new neutral check.
enum class DeviceReadStatus { unavailable, pending, available, resumed };

class DeviceSampleSource {
public:
    virtual ~DeviceSampleSource() = default;
    virtual std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>& buttons,
                                               DeviceReadStatus* status) = 0;
};

class DeviceReadContinuity {
public:
    using Clock = std::chrono::steady_clock;

    DeviceReadStatus pending(Clock::time_point now) noexcept;
    DeviceReadStatus available(Clock::time_point sampledAt) noexcept;
    void reset() noexcept;

private:
    Clock::time_point lastSampled_{};
    bool pending_{};
};

// Use each instance from one thread, with a top-level window of this process.
class DirectInput final : public DeviceSampleSource {
public:
    explicit DirectInput(HWND window, DirectInputDiagnostic diagnostic = nullptr,
                         DirectInputReadObserver readObserver = nullptr, void* observerContext = nullptr,
                         DirectInputActiveCall* activeCall = nullptr);
    ~DirectInput();
    DirectInput(const DirectInput&) = delete;
    DirectInput& operator=(const DirectInput&) = delete;

    bool ready() const noexcept;
    std::vector<DeviceInfo> devices() const;
    // Start a sampling cycle when several readers use the same device states.
    void beginPoll() noexcept;
    // Only readers that validate a saved button mapping need its availability checked.
    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>& buttons = {},
                                       DeviceReadStatus* status = nullptr) override;
    // Synchronous reads can limit layout discovery to a selection of commands.
    std::optional<DeviceSample> sampleConfigured(const std::string& id, const DeviceObjectSelection& wanted,
                                                 DeviceReadStatus& status);
    // Driving reads discover configured objects independently of the live report.
    std::optional<DeviceSample> sampleForDriving(const std::string& id, const DeviceObjectSelection& wanted,
                                                 DeviceReadStatus& status);
    // Output is limited to the calibrated actuator and expires if calls stop.
    // Initialization can continue for a newer valid request for the same connection.
    // Starting an effect always requires the current request.
    // Only a newly submitted effect returns an accepted signed fraction of nominal force.
    std::optional<float> setWheelForce(std::string_view id, std::size_t axis, float force,
                                      WheelForceGuard guard = {});
    WheelForceState wheelForceState() const noexcept;
    void stopWheelForce() noexcept;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chrome_axis
