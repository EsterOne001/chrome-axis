#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace chrome_axis {

enum class DiagnosticMode : std::uint32_t {
    waitingForDriver, gameNotForeground, waitingForVehicleInput, usbDisabled, profileUnavailable, usb, simulation
};

enum class DiagnosticAxisState : std::uint32_t {
    unassigned, deviceUnavailable, axisUnavailable, waitingForNeutral, armed, awaitingProfile, inactive, simulated, sampleExpired
};

enum class DiagnosticAccessoryState : std::uint32_t {
    unconfigured, waiting, ready, profileError, inactive, blocked, deviceUnavailable, inputUnavailable, sampleExpired
};

enum class DiagnosticFeedbackState : std::uint32_t { disabled, waiting, active, unavailable, inactive };

enum class DiagnosticDelivery : std::uint32_t { unavailable, applied, gameKept };

struct DiagnosticAxis {
    DiagnosticAxisState state{DiagnosticAxisState::inactive};
    float value{};
    std::uint64_t appliedAt{}; // GetTickCount64() when an input was last written to the vehicle.
    float inputValue{}; // Calibrated physical reading, before neutral gating.
    float requestedValue{}; // Value the game hook considered, including steering compensation.
    std::uint32_t inputAvailable{};
    std::uint32_t requestedAvailable{};
    DiagnosticDelivery delivery{DiagnosticDelivery::unavailable};
};

struct DiagnosticSnapshot {
    DiagnosticMode mode{DiagnosticMode::waitingForDriver};
    std::array<DiagnosticAxis, 3> axes{}; // Steering, throttle, brake.
    std::array<char, 257> profile{}; // UTF-8 profile name. Empty for the default profile.
    std::uint32_t profileLoaded{};
    std::uint32_t accessoryCount{};
    std::uint32_t accessoryArmed{};
    std::uint32_t lastAccessoryKey{};
    std::uint64_t accessorySentAt{};
    DiagnosticAccessoryState accessories{DiagnosticAccessoryState::inactive};
    DiagnosticFeedbackState feedback{DiagnosticFeedbackState::disabled};
};

enum class DiagnosticConnection { unavailable, disconnected, stale, connected };

struct DiagnosticEvent {
    std::uint64_t sampledAt{};
    DiagnosticMode mode{DiagnosticMode::waitingForDriver};
    std::array<DiagnosticAxisState, 3> axes{};
    DiagnosticAccessoryState accessories{DiagnosticAccessoryState::inactive};
    DiagnosticFeedbackState feedback{DiagnosticFeedbackState::disabled};
};

struct DiagnosticRead {
    DiagnosticConnection connection{DiagnosticConnection::unavailable};
    DiagnosticSnapshot snapshot{};
    std::uint64_t sampledAt{};
    DiagnosticSnapshot lastDrive{};
    std::uint64_t lastDriveAt{};
    std::array<DiagnosticEvent, 8> events{}; // Oldest first. Only status transitions are kept.
    std::uint32_t eventCount{};
};

struct DiagnosticFrame;

// Identify the same installation through directory aliases and links.
std::wstring installationChannelName(const std::filesystem::path& gameRoot, std::wstring_view prefix);

// One publisher per installation. Reads and writes never wait on the other process.
class DiagnosticPublisher {
public:
    explicit DiagnosticPublisher(const std::filesystem::path& gameRoot);
    ~DiagnosticPublisher();
    DiagnosticPublisher(const DiagnosticPublisher&) = delete;
    DiagnosticPublisher& operator=(const DiagnosticPublisher&) = delete;

    // A nonzero driving session identifies a snapshot taken in the driver's seat with game focus.
    void publish(const DiagnosticSnapshot& snapshot, std::uint64_t drivingSession = 0) noexcept;

private:
    HANDLE owner_{};
    HANDLE mutex_{};
    HANDLE mapping_{};
    DiagnosticFrame* frame_{};
    DiagnosticRead report_{};
    std::uint64_t drivingSession_{};
};

class DiagnosticReader {
public:
    explicit DiagnosticReader(const std::filesystem::path& gameRoot);
    ~DiagnosticReader();
    DiagnosticReader(const DiagnosticReader&) = delete;
    DiagnosticReader& operator=(const DiagnosticReader&) = delete;

    DiagnosticRead read() noexcept;

private:
    std::wstring channel_;
    std::wstring mutexName_;
    HANDLE mutex_{};
    HANDLE mapping_{};
    const DiagnosticFrame* frame_{};
    DiagnosticRead last_{};
};

} // namespace chrome_axis
