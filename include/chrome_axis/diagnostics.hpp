#pragma once

#include "chrome_axis/accessories.hpp"

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

enum class DiagnosticCommandState : std::uint8_t {
    inactive, waitingForVehicle, waitingForNeutral, ready, deviceUnavailable, inputUnavailable,
    temporarilyUnavailable, sampleExpired
};

enum class DiagnosticCommandOutput : std::uint8_t {
    none, held, pulsing, blocked, failed, nativeReady, nativePending, nativeEngaged, nativeRefused, nativePaused
};

struct DiagnosticAccessoryCommand {
    AccessoryKind kind{};
    ShifterRole role{};
    AccessoryActivation activation{};
    std::uint32_t index{};
    std::uint32_t key{};
    std::int32_t rest{};
    std::int32_t engaged{};
    DiagnosticCommandState state{};
    DiagnosticCommandOutput output{};
    std::uint8_t active{};
    std::uint8_t activeAvailable{};
    std::uint8_t travelAvailable{};
    float travel{};
    std::uint64_t lastSentAt{}; // GetTickCount64() when Windows accepted the key-down.
    std::uint32_t handbrakeCeilingPercent{};
    float handbrakeRequested{};
    float handbrakeFinal{}; // Vehicle command after arbitration with the native handbrake.
    std::uint8_t handbrakeAvailable{};
};

enum class DiagnosticFeedbackState : std::uint32_t { disabled, waiting, active, unavailable, inactive, neutral, waitingForMotion };

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
    float finalValue{}; // Pedal command after USB and native input arbitration in the game hook.
    std::uint32_t finalAvailable{};
};

struct DiagnosticFeedbackTelemetry {
    std::uint64_t motionAgeMicros{};
    std::uint64_t submittedAt{}; // GetTickCount64() when the driver accepted the effect.
    float rawRequested{}; // Signed force before limiting, in the same nominal units.
    float requested{}; // Signed force after limiting, relative to the DirectInput nominal force.
    float submitted{}; // Last accepted signed effect in the same units.
    std::uint8_t motionAvailable{};
    std::uint8_t requestAvailable{};
    std::uint8_t clipped{};
    std::uint8_t impactClipped{}; // This saturation was caused by a collision or landing effect.
    std::uint8_t airborne{};
    std::uint8_t skidding{};
};

enum class DiagnosticShiftState : std::uint8_t {
    automatic, ready, unavailable, outsideRange, stopToChangeDirection, alreadyEngaged,
    rejected, accepted, neutralTransition, expired, recalibrationNeeded, selectorUnavailable
};

struct DiagnosticTransmission {
    bool configured{};
    bool manual{};
    int selectedGear{-2};
    int requestedGear{-2};
    int engagedGear{-2};
    DiagnosticShiftState shift{};

    bool operator==(const DiagnosticTransmission&) const = default;
};

struct DiagnosticSnapshot {
    DiagnosticMode mode{DiagnosticMode::waitingForDriver};
    std::array<DiagnosticAxis, 3> axes{}; // Steering, throttle, brake.
    std::uint64_t pedalSampledAt{}; // GetTickCount64() of the two contemporaneous final pedal commands.
    std::array<char, 257> profile{}; // UTF-8 profile name. Empty for the default profile.
    std::uint32_t profileLoaded{};
    std::uint32_t accessoryCount{};
    std::array<DiagnosticAccessoryCommand, maxAccessoryBindings> accessoryCommands{};
    std::uint32_t accessoryArmed{};
    std::uint32_t lastAccessoryKey{};
    std::uint64_t accessorySentAt{};
    DiagnosticAccessoryState accessories{DiagnosticAccessoryState::inactive};
    DiagnosticFeedbackState feedback{DiagnosticFeedbackState::disabled};
    DiagnosticFeedbackTelemetry feedbackData{};
    DiagnosticTransmission transmission{};
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
    PerformanceTiming motionAge;
    std::uint64_t expiredReads{};
    std::uint64_t transientReads{};
    std::uint64_t forceSamples{};
    std::uint64_t clippedSamples{};
    std::uint64_t impactClippedSamples{};
    std::uint64_t submittedEffects{};
    float peakRequestedForce{}; // Absolute normalized peak before limiting.
    float peakLimitedForce{}; // Absolute normalized peak after limiting.
    float peakSubmittedForce{}; // Absolute normalized peak of the effects accepted by the driver.
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
