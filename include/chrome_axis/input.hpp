#pragma once

#include <windows.h>

#include <array>
#include <bitset>
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

struct DeviceInfo {
    std::string id;
    std::string name;
    std::array<bool, axisCount> axes{};
    std::bitset<buttonCount> buttons{};
    std::array<bool, povCount> povs{};
    std::array<bool, axisCount> forceFeedbackAxes{};
};

struct DeviceSample {
    std::array<long, axisCount> axes{};
    std::bitset<buttonCount> buttons{};
    std::array<bool, axisCount> axesPresent{};
    // Angles clockwise from up, in hundredths of a degree. Absent hats stay centered.
    std::array<std::uint16_t, povCount> povs{povCentered, povCentered, povCentered, povCentered};
    std::bitset<buttonCount> buttonsPresent{};
    std::array<bool, povCount> povsPresent{};
};

const char* axisName(std::size_t index) noexcept;
bool isDeviceId(std::string_view id) noexcept;

// An empty device ID refers to initialization. Availability is separate from HRESULT
// because DirectInput can report a detached device with a success-class HRESULT.
using DirectInputDiagnostic = void (*)(std::string_view deviceId, std::string_view stage,
    HRESULT result, bool available);

enum class WheelForceState { waiting, active, unavailable };

// Use each instance from one thread, with a top-level window of this process.
class DirectInput {
public:
    explicit DirectInput(HWND window, DirectInputDiagnostic diagnostic = nullptr);
    ~DirectInput();
    DirectInput(const DirectInput&) = delete;
    DirectInput& operator=(const DirectInput&) = delete;

    bool ready() const noexcept;
    std::vector<DeviceInfo> devices() const;
    // Start a sampling cycle when several readers use the same device states.
    void beginPoll() noexcept;
    // Only readers that validate a saved button mapping need its availability checked.
    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>& buttons = {});
    // Output is limited to the calibrated actuator and expires if calls stop.
    // The optional guard is called synchronously to recheck context around driver calls.
    void setWheelForce(const std::string& id, std::size_t axis, float force,
                       bool (*stillCurrent)(const void*) noexcept = nullptr, const void* context = nullptr);
    WheelForceState wheelForceState() const noexcept;
    void stopWheelForce() noexcept;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chrome_axis
