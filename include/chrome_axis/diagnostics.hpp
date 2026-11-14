#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace chrome_axis {

enum class DiagnosticMode : std::uint32_t {
    waitingForDriver, gameNotForeground, waitingForVehicleInput, usbDisabled, profileUnavailable, usb, simulation
};

enum class DiagnosticAxisState : std::uint32_t {
    unassigned, deviceUnavailable, axisUnavailable, waitingForNeutral, armed, awaitingProfile, inactive, simulated,
    sampleExpired, temporarilyUnavailable
};

enum class DiagnosticAccessoryState : std::uint32_t {
    unconfigured, waiting, ready, profileError, inactive, blocked, deviceUnavailable, inputUnavailable, sampleExpired,
    temporarilyUnavailable
};

enum class DiagnosticFeedbackState : std::uint32_t { disabled, waiting, active, unavailable, inactive };

enum class DiagnosticDelivery : std::uint32_t { unavailable, applied, gameKept };

struct DiagnosticAxis {
    DiagnosticAxisState state{DiagnosticAxisState::inactive};
    float value{}; // Last successful write, which can precede the reading below.
    std::uint64_t appliedAt{}; // GetTickCount64() when an input was last written to the vehicle.
    float inputValue{}; // Calibrated physical reading, before neutral gating.
    float requestedValue{}; // Value the game hook considered from that same reading, including steering compensation.
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

enum class PerformanceState : std::uint32_t { idle, capturing, complete };

struct PerformanceTiming {
    std::uint64_t samples{};
    std::uint64_t totalMicros{};
    std::uint64_t maxMicros{};

    void add(std::uint64_t micros) noexcept;
};

struct PerformanceReport {
    PerformanceState state{PerformanceState::idle};
    std::uint64_t requestId{};
    std::uint64_t startedAt{}; // GetTickCount64() at start.
    std::uint64_t endedAt{};
    PerformanceTiming usbRead;
    PerformanceTiming feedback;
    PerformanceTiming inputAge;
    std::uint64_t expiredReads{};
    std::uint64_t transientReads{};
};

struct PerformanceRequest {
    std::uint64_t id{};
    std::uint64_t leaseUntil{}; // GetTickCount64() deadline for an active capture.
    std::uint32_t enabled{};
};

struct DiagnosticRead {
    DiagnosticConnection connection{DiagnosticConnection::unavailable};
    DiagnosticSnapshot snapshot{};
    std::uint64_t sampledAt{};
    DiagnosticSnapshot lastDrive{};
    std::uint64_t lastDriveAt{};
    std::array<DiagnosticEvent, 8> events{}; // Oldest first. Only status transitions are kept.
    std::uint32_t eventCount{};
    PerformanceReport performance{};
};

struct DiagnosticFrame;

// One publisher per installation. Reads and writes never wait on the other process.
class DiagnosticPublisher {
public:
    explicit DiagnosticPublisher(const std::filesystem::path& gameRoot);
    ~DiagnosticPublisher();
    DiagnosticPublisher(const DiagnosticPublisher&) = delete;
    DiagnosticPublisher& operator=(const DiagnosticPublisher&) = delete;

    // A nonzero driving session identifies a snapshot taken in the driver's seat with game focus.
    void publish(const DiagnosticSnapshot& snapshot, std::uint64_t drivingSession = 0,
                 const PerformanceReport& performance = {}) noexcept;
    std::optional<PerformanceRequest> performanceRequest() const noexcept;

private:
    HANDLE owner_{};
    HANDLE mutex_{};
    HANDLE mapping_{};
    DiagnosticFrame* frame_{};
    DiagnosticRead report_{};
    std::uint64_t drivingSession_{};
    std::uint64_t publisherId_{};
};

class DiagnosticReader {
public:
    explicit DiagnosticReader(const std::filesystem::path& gameRoot);
    ~DiagnosticReader();
    DiagnosticReader(const DiagnosticReader&) = delete;
    DiagnosticReader& operator=(const DiagnosticReader&) = delete;

    DiagnosticRead read() noexcept;
    // Submit a command for this reader's capture. A zero result means no request was sent.
    std::uint64_t requestPerformanceCapture(bool enabled) noexcept;
    bool ownsPerformanceCapture() const noexcept { return captureOwner_ != 0; }

private:
    std::wstring channel_;
    std::wstring mutexName_;
    HANDLE mutex_{};
    HANDLE mapping_{};
    DiagnosticFrame* frame_{};
    DiagnosticRead last_{};
    std::uint64_t publisherId_{};
    std::uint64_t captureOwner_{};
};

} // namespace chrome_axis
