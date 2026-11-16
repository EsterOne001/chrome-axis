#pragma once

#include "chrome_axis/accessories.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>

namespace chrome_axis {

enum class DiagnosticMode : std::uint32_t {
    waitingForDriver, gameNotForeground, waitingForVehicleInput, usbDisabled, profileUnavailable, usb, simulation
};

constexpr bool isDrivingCaptureMode(DiagnosticMode mode) noexcept
{
    return mode == DiagnosticMode::usb || mode == DiagnosticMode::simulation ||
        mode == DiagnosticMode::usbDisabled || mode == DiagnosticMode::profileUnavailable;
}

constexpr bool canRetainDrivingSample(DiagnosticMode mode, bool usbReady, bool freshPartialUsbDelivery) noexcept
{
    return mode == DiagnosticMode::simulation || mode == DiagnosticMode::usbDisabled ||
        (mode == DiagnosticMode::usb && (usbReady || freshPartialUsbDelivery));
}

enum class DiagnosticAxisState : std::uint32_t {
    unassigned, deviceUnavailable, axisUnavailable, waitingForNeutral, armed, awaitingProfile, inactive, simulated,
    sampleExpired, temporarilyUnavailable
};

enum class DiagnosticAccessoryState : std::uint32_t {
    unconfigured, waiting, ready, profileError, inactive, blocked, deviceUnavailable, inputUnavailable, sampleExpired,
    temporarilyUnavailable, axisConflict, modifierBlocked, selectorAmbiguous
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
    rejected, accepted, neutralTransition, expired, recalibrationNeeded, selectorUnavailable, pending, clutchRequired,
    selectorAmbiguous
};

struct DiagnosticTransmission {
    bool configured{};
    bool manual{};
    int selectedGear{-2};
    int requestedGear{-2};
    int engagedGear{-2};
    DiagnosticShiftState shift{};
    bool sequential{};
    std::uint32_t shortcutKey{};
    std::uint64_t shortcutPressedAt{}; // GetTickCount64() of the last recognized press.
    bool hasRequestedMode{};
    bool requestedManual{};
    bool shortcutAvailable{};
    std::uint64_t gearboxSampledAt{}; // GetTickCount64() of the observed gear, or zero if unavailable.

    bool operator==(const DiagnosticTransmission&) const = default;
};

enum class DiagnosticClutchRest : std::uint8_t { unknown, inRange, outOfRange };

enum class DiagnosticClutchBlocker : std::uint8_t {
    none, waitingForNeutral, signalPending, signalUnavailable, belowThreshold, retryPaddle
};

struct DiagnosticClutch {
    float input{};
    float applied{};
    bool configured{};
    bool inputAvailable{};
    bool assistanceEnabled{};
    bool probeSupported{};
    bool appliedAvailable{};
    bool probeActive{};
    float rawTravel{}; // Signed calibrated fraction before removing the dead zone.
    DiagnosticCommandState signal{};
    DiagnosticClutchRest rest{}; // Evaluated before converting rawTravel to float.
    DiagnosticClutchBlocker blocker{};
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
    DiagnosticClutch clutch{};
};

enum class DiagnosticConnection { unavailable, disconnected, stale, connected };

struct DiagnosticEvent {
    std::uint64_t sampledAt{};
    DiagnosticMode mode{DiagnosticMode::waitingForDriver};
    std::array<DiagnosticAxisState, 3> axes{};
    DiagnosticAccessoryState accessories{DiagnosticAccessoryState::inactive};
    DiagnosticFeedbackState feedback{DiagnosticFeedbackState::disabled};
};

enum class DiagnosticUsbEventKind : std::uint8_t {
    deviceSlow, devicePending, deviceUnavailable, deviceRecovered, pollExpired, pollGap,
    commandsWaiting, commandsReady, contextChanged, menuPaused, directInputReopened, discreteOverflow,
    layoutPending, layoutAcquireDenied, layoutMissing, layoutRecovered
};

struct DiagnosticUsbBlocker {
    AccessoryKind kind{};
    ShifterRole role{};
    std::uint32_t index{};
    std::uint32_t key{};
    DiagnosticCommandState state{};
    bool clutch{};
    bool progressiveHandbrake{};

    bool operator==(const DiagnosticUsbBlocker&) const = default;
};

struct DiagnosticUsbEvent {
    std::uint64_t sampledAt{}; // GetTickCount64() at detection.
    std::uint64_t durationMicros{};
    std::array<char, 39> deviceId{}; // Lookup key for the application. Omit from exported text.
    std::array<char, 40> operation{}; // DirectInput API name.
    std::uint32_t result{};
    DiagnosticUsbEventKind kind{};
    DiagnosticMode mode{};
    std::array<DiagnosticAxisState, 3> axes{};
    DiagnosticAccessoryState accessories{};
    std::uint32_t accessoryArmed{};
    std::uint32_t accessoryCount{};
    std::uint8_t deviceAxisMask{};
    std::uint8_t deviceAccessoryCount{};
    std::array<DiagnosticUsbBlocker, 4> blockers{};
    std::uint32_t blockerCount{}; // Total, including any entries omitted from blockers.
};

enum class PerformanceState : std::uint32_t { idle, capturing, complete };

struct PerformanceTiming {
    static constexpr std::array<std::uint64_t, 20> bucketLimits{
        100, 250, 500, 750, 1000, 2000, 3000, 4000, 5000, 7500, 10000, 15000, 20000, 30000, 50000,
        75000, 100000, 200000, 500000, 1000000
    };
    std::uint64_t samples{};
    std::uint64_t totalMicros{};
    std::uint64_t maxMicros{};
    std::array<std::uint64_t, bucketLimits.size() + 1> buckets{};

    void add(std::uint64_t micros) noexcept;
    // Approximate upper bound, limited to the observed maximum.
    std::uint64_t percentile(unsigned int percent) const noexcept;
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
    std::uint64_t usbIntervalOverruns{};
    std::uint64_t feedbackIntervalOverruns{};
    float peakContinuousForce{};
    int referenceFeedbackGain{};
    std::array<char, 257> feedbackProfile{};
};

inline constexpr std::size_t diagnosticCaptureCapacity = 4096;

struct DrivingCaptureSample {
    std::uint64_t sampledAt{}; // GetTickCount64().
    DiagnosticMode mode{};
    std::array<char, 64> vehicle{}; // Optional UTF-8 vehicle label.
    float speedMps{};
    float rpm{};
    float throttle{};
    float brake{};
    bool speedAvailable{};
    bool rpmAvailable{};
    bool throttleAvailable{};
    bool brakeAvailable{};
    DiagnosticTransmission transmission{};
    DiagnosticClutch clutch{};
    std::uint64_t shiftDelayMicros{};
    bool shiftDelayAvailable{};
    std::array<DiagnosticAxisState, 3> axes{};
    float steering{};
    float handbrake{};
    float feedbackForce{};
    bool steeringAvailable{};
    bool handbrakeAvailable{};
    bool feedbackForceAvailable{};
};

struct DiagnosticCaptureRecord {
    DrivingCaptureSample driving{};
    DiagnosticUsbEvent usb{};
    bool usbIncident{};
};

struct DiagnosticCaptureBatch {
    std::size_t count{};
    std::uint64_t dropped{};
    bool finished{};
    bool available{};
};

void writeDiagnosticCaptureHeader(std::ostream& output);
void writeDiagnosticCaptureRows(std::ostream& output, std::span<const DiagnosticCaptureRecord> records,
                                std::uint64_t startedAt, std::uint64_t dropped = 0);
void writeDiagnosticCaptureConnection(std::ostream& output, std::uint64_t startedAt,
                                      std::uint64_t sampledAt, bool connected);

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
    std::array<DiagnosticUsbEvent, 32> usbEvents{}; // Oldest first, retained across driving sessions.
    std::uint32_t usbEventCount{};
    PerformanceReport performance{};
    std::uint32_t drivingCaptureSamples{}; // Only visible to the owning reader.
};

struct DiagnosticFrame;

// One publisher per installation. Reads and writes never wait on the other process.
class DiagnosticPublisher {
public:
    explicit DiagnosticPublisher(const std::filesystem::path& gameRoot);
    ~DiagnosticPublisher();
    DiagnosticPublisher(const DiagnosticPublisher&) = delete;
    DiagnosticPublisher& operator=(const DiagnosticPublisher&) = delete;

    // Always publish live status. Retain this reading only when the driving commands are usable.
    void publish(const DiagnosticSnapshot& snapshot, std::uint64_t drivingSession = 0,
                 const PerformanceReport& performance = {}, bool validDrivingSample = true) noexcept;
    void recordUsbEvent(const DiagnosticUsbEvent& event) noexcept;
    void recordDrivingSample(std::uint64_t captureId, const DrivingCaptureSample& sample) noexcept;
    std::optional<PerformanceRequest> performanceRequest() noexcept;

private:
    void queueCaptureRecord(const DiagnosticCaptureRecord& row) noexcept;
    void flushCaptureRecords() noexcept; // Shared frame mutex held.

    HANDLE owner_{};
    HANDLE mutex_{};
    HANDLE mapping_{};
    DiagnosticFrame* frame_{};
    DiagnosticRead report_{};
    std::uint64_t drivingSession_{};
    std::uint64_t publisherId_{};
    PerformanceRequest observedRequest_{};
    DrivingCaptureSample lastCaptureSample_{};
    std::array<DiagnosticCaptureRecord, 256> pending_{};
    std::size_t pendingHead_{};
    std::size_t pendingCount_{};
    std::uint64_t pendingId_{};
    std::uint64_t pendingDropped_{};
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
    bool captureSlotOccupied() const noexcept {
        return last_.connection == DiagnosticConnection::connected && captureSlotOccupied_;
    }
    // Caller supplies a nonempty buffer. False availability is transient unless finished is also true.
    DiagnosticCaptureBatch drainCapture(std::span<DiagnosticCaptureRecord> output) noexcept;

private:
    bool publisherPresent() const noexcept; // Shared frame mutex held.

    std::wstring channel_;
    std::wstring mutexName_;
    std::wstring publisherName_;
    HANDLE mutex_{};
    HANDLE mapping_{};
    DiagnosticFrame* frame_{};
    DiagnosticRead last_{};
    std::uint64_t publisherId_{};
    std::uint64_t captureOwner_{};
    std::uint64_t captureId_{};
    std::uint64_t capturePublisherId_{};
    std::uint64_t captureRead_{};
    std::uint64_t nextSequence_{};
    bool captureSlotOccupied_{};
};

} // namespace chrome_axis
