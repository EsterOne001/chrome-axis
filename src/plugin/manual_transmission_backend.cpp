#include "manual_transmission_backend.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace manual_backend {
namespace {
using namespace detail;

// Cyberpunk 2077 2.5, executable 3.0.88.46235. These routines and fields were
// verified against the local x64 game image. Reject other layouts before hooking.
constexpr std::uintptr_t stepRva = 0x6E2E6C;
constexpr std::uintptr_t shiftRva = 0x81DB9C;
constexpr std::uintptr_t shiftCallRva = 0x6E32BA;
constexpr std::uintptr_t gearboxOwnerLinkRva = 0x81DB1D;
constexpr std::uintptr_t bodyLinkRva = 0x6E2F41;
constexpr std::uintptr_t bodyVelocityLoadRva = 0x6E4641;
constexpr std::uintptr_t engineLinkRva = 0x6E2EE4;
constexpr std::uintptr_t engineRpmStoreRva = 0x6E0318;
constexpr std::uintptr_t shiftCooldownLoadRva = 0x81DBD9;
constexpr std::uintptr_t neutralRequestRva = 0x6E32CC;
constexpr std::uintptr_t reverseRequestRva = 0x6E3273;
constexpr std::array<std::uint8_t, 20> stepBytes{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68,
    0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr std::array<std::uint8_t, 24> shiftBytes{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
    0xD9, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x8B, 0x89, 0xC8, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> shiftCallBytes{
    0xE8, 0xDD, 0xA8, 0x13, 0x00, 0xE9, 0x47, 0xFC, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 10> gearboxOwnerLinkBytes{
    0x48, 0x8B, 0x89, 0xC8, 0x0B, 0x00, 0x00, 0x48, 0x89, 0x39};
constexpr std::array<std::uint8_t, 7> bodyLinkBytes{0x49, 0x8B, 0xAA, 0xF0, 0x05, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> bodyVelocityLoadBytes{
    0x41, 0x0F, 0x10, 0x58, 0x30, 0x0F, 0x28, 0xC3};
constexpr std::array<std::uint8_t, 7> engineLinkBytes{0x4C, 0x8B, 0x99, 0xD0, 0x0B, 0x00, 0x00};
constexpr std::array<std::uint8_t, 14> engineRpmStoreBytes{
    0xF3, 0x0F, 0x5F, 0x53, 0x68, 0xF3, 0x0F, 0x5D, 0xD0, 0xF3, 0x0F, 0x11, 0x53, 0x14};
constexpr std::array<std::uint8_t, 10> shiftCooldownLoadBytes{
    0x0F, 0x57, 0xF6, 0x0F, 0x2F, 0xB3, 0xB4, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> neutralRequestBytes{0x83, 0xCA, 0xFF, 0xEB, 0xE0};
constexpr std::array<std::uint8_t, 4> reverseRequestBytes{0x33, 0xD2, 0xEB, 0x3D};

constexpr auto ownerOffset = 0x0;
constexpr auto vehicleDriveBodyOffset = 0x5F0;
constexpr auto bodyVelocityOffset = 0x30;
constexpr auto vehicleGearboxOffset = 0xBC8;
constexpr auto vehicleEngineOffset = 0xBD0;
constexpr auto engineRpmOffset = 0x14;
constexpr auto gearCountOffset = 0x198;
constexpr auto shiftTimeOffset = 0x1AC;
constexpr auto shiftCooldownOffset = 0x1B4;
constexpr auto targetGearOffset = 0x1C4;
constexpr auto engagedGearOffset = 0x1C8;
constexpr auto leaseDurationMs = 250ULL;
constexpr auto requestLifetimeMs = 900ULL;
constexpr auto engagementLifetimeMs = 2500ULL;
constexpr auto telemetryIntervalMs = 50ULL;

using Step = void (*)(void*, float);
using Shift = bool (*)(void*, int, bool, bool);

RED4ext::v1::PluginHandle hookPlugin{};
const RED4ext::v1::Sdk* hookSdk{};
void* stepTarget{};
void* shiftTarget{};
void* originalStep{};
void* originalShift{};
bool engineRpmAvailable{};
std::atomic<bool> installed{false};
std::atomic<void*> publishedVehicle{nullptr};
SRWLOCK stateLock = SRWLOCK_INIT;

struct ShiftRequest {
    std::optional<int> absolute;
    int delta{};
    ULONGLONG expiresAt{};
};

struct SharedState {
    void* vehicle{};
    ULONGLONG expiresAt{};
    std::uint64_t generation{};
    std::uint64_t selectionRevision{};
    std::uint64_t reverseIssueSerial{};
    std::optional<int> selectedGear;
    std::optional<int> heldGear;
    std::optional<ShiftRequest> request;
    std::uint64_t dispatchExpiresAt{};
    std::uint64_t engagementExpiresAt{};
    // Still mounted during menus even when the input lease and its observations are invalid.
    DirectionHistory directionHistory;
    VehicleHeading heading;
    bool enabled{};
    bool captureTelemetry{};
    Snapshot view{};
};

SharedState state;

class StateGuard {
public:
    StateGuard() noexcept { AcquireSRWLockExclusive(&stateLock); }
    ~StateGuard() { ReleaseSRWLockExclusive(&stateLock); }
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;
};

struct ManualStep {
    void* gearbox{};
    void* owner{};
    std::uint64_t generation{};
};

thread_local ManualStep activeStep;

class StepScope {
public:
    explicit StepScope(ManualStep step) noexcept : previous_(std::exchange(activeStep, step)) {}
    ~StepScope() { activeStep = previous_; }
    StepScope(const StepScope&) = delete;
    StepScope& operator=(const StepScope&) = delete;

private:
    ManualStep previous_;
};

template<class T>
T readField(const void* object, std::size_t offset) noexcept {
    T result{};
    std::memcpy(&result, static_cast<const std::byte*>(object) + offset, sizeof(result));
    return result;
}

bool gameForeground() noexcept {
    DWORD process{};
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}

bool supportedGame() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return false;

    DWORD ignored{};
    const auto size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (size == 0)
        return false;

    std::vector<std::byte> info(size);
    UINT valueSize{};
    void* value{};
    if (!GetFileVersionInfoW(path.data(), 0, size, info.data()) ||
        !VerQueryValueW(info.data(), L"\\", &value, &valueSize) || valueSize < sizeof(VS_FIXEDFILEINFO))
        return false;

    const auto& version = *static_cast<const VS_FIXEDFILEINFO*>(value);
    return version.dwSignature == 0xFEEF04BD && version.dwFileVersionMS == MAKELONG(0, 3) &&
           version.dwFileVersionLS == MAKELONG(46235, 88);
}

template<std::size_t N>
void* verifiedAddress(HMODULE module, std::uintptr_t rva, const std::array<std::uint8_t, N>& bytes) noexcept {
    if (!module)
        return nullptr;

    auto* address = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(module) + rva);
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.AllocationBase != module ||
        memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 || memory.RegionSize < N ||
        reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(memory.BaseAddress)
            > memory.RegionSize - N)
        return nullptr;

    switch (memory.Protect & 0xFF) {
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return std::memcmp(address, bytes.data(), N) == 0 ? address : nullptr;
    default:
        return nullptr;
    }
}

bool liveLease(const void* owner, std::uint64_t generation) noexcept {
    return state.vehicle == owner && state.generation == generation && GetTickCount64() < state.expiresAt;
}

bool filterAutomatic(void* gearbox, int target, bool immediate, bool bypassCooldown) noexcept {
    const auto original = std::bit_cast<Shift>(originalShift);
    if (gearbox && activeStep.gearbox == gearbox && !immediate && gameForeground()) {
        const auto engaged = readField<int>(gearbox, engagedGearOffset);
        const auto entries = readField<int>(gearbox, gearCountOffset);
        StateGuard guard;
        const bool live = state.enabled && liveLease(activeStep.owner, activeStep.generation);
        if (live) {
            auto held = state.heldGear;
            if (!held && state.selectedGear && (engaged == neutralGear || state.selectedGear == neutralGear))
                held = neutralGear;
            if (suppressAutomatic(engaged, target, entries, bypassCooldown, held))
                return false;
        }
    }
    return original(gearbox, target, immediate, bypassCooldown);
}

std::optional<std::array<float, 3>> vehicleVelocity(const void* vehicle) noexcept {
    const auto* driveBody = readField<void*>(vehicle, vehicleDriveBodyOffset);
    if (!driveBody)
        return std::nullopt;
    // The native planar speed routine reads this same velocity vector before projecting it.
    return readField<std::array<float, 3>>(driveBody, bodyVelocityOffset);
}

std::optional<float> speedOf(const std::optional<std::array<float, 3>>& velocity) noexcept {
    if (!velocity)
        return std::nullopt;
    const float absoluteSpeed = std::hypot((*velocity)[0], (*velocity)[1], (*velocity)[2]);
    return std::isfinite(absoluteSpeed) ? std::optional{absoluteSpeed} : std::nullopt;
}

void sampleTelemetry(const void* vehicle, std::uint64_t now) noexcept {
    if (!state.captureTelemetry || (state.view.telemetrySampledAt &&
        now - state.view.telemetrySampledAt < telemetryIntervalMs))
        return;

    state.view.telemetrySampledAt = now;
    state.view.speedMetersPerSecond = speedOf(vehicleVelocity(vehicle));
    state.view.engineRpm.reset();
    if (const auto* engine = engineRpmAvailable ? readField<void*>(vehicle, vehicleEngineOffset) : nullptr) {
        const float rpm = readField<float>(engine, engineRpmOffset);
        if (std::isfinite(rpm) && rpm >= 0.0f)
            state.view.engineRpm = rpm;
    }
}

void resetSelection() noexcept {
    ++state.selectionRevision;
    state.selectedGear.reset();
    state.heldGear.reset();
    state.request.reset();
    state.dispatchExpiresAt = 0;
    state.engagementExpiresAt = 0;
    if (state.view.requestPending) {
        state.view.requestPending = false;
        state.view.outcome = "request_cancelled";
    }
}

// The input and status callbacks keep timeouts working when the gearbox callback is absent.
void expireRequests(std::uint64_t now) noexcept {
    const bool engagementExpired = requestExpired(state.engagementExpiresAt, now);
    const auto requestDeadline = state.request ? state.request->expiresAt : state.dispatchExpiresAt;
    if (!engagementExpired && !requestExpired(requestDeadline, now))
        return;

    ++state.selectionRevision;
    state.request.reset();
    state.dispatchExpiresAt = 0;
    state.engagementExpiresAt = 0;
    state.view.requestPending = false;
    state.view.outcome = engagementExpired ? "engagement_unconfirmed" : "request_expired";
}

void queueShift(std::optional<int> absolute, int delta, std::uint64_t now) noexcept {
    state.request = ShiftRequest{absolute, delta, now + requestLifetimeMs};
    state.engagementExpiresAt = 0;
    state.view.requestPending = true;
    state.view.requestedAt = now;
    state.view.requestedGear = absolute.value_or(unavailableGear);
    state.view.engagementDelayMs.reset();
    state.view.outcome = "request_pending";
}

struct ReadyRequest {
    ShiftRequest request;
    std::uint64_t selectionRevision{};
    Direction lastDirection{Direction::unknown};
    std::optional<std::array<float, 3>> forward;
};

std::optional<ReadyRequest> takeReadyRequest(const void* owner, std::uint64_t generation,
                                             int engaged, int target, int entries, const void* gearbox) noexcept {
    StateGuard guard;
    if (!liveLease(owner, generation))
        return std::nullopt;

    const auto now = GetTickCount64();
    const bool observedBeforeStep = freshGearbox(state.view, now);
    const bool validGear = supportedGear(engaged, entries);
    const auto direction = state.directionHistory.observe(owner, engaged, target, entries);
    state.view.engagedGear = validGear ? engaged : unavailableGear;
    state.view.targetGear = supportedGear(target, entries) ? target : unavailableGear;
    state.view.gearCount = validGear ? entries : 0;
    state.view.sampledAt = validGear ? now : 0;
    sampleTelemetry(owner, now);
    expireRequests(now);
    if (!state.enabled && !state.view.reverseIssued)
        return std::nullopt;
    if (!state.request && !state.engagementExpiresAt && !state.view.reverseIssued)
        return std::nullopt;

    const float shiftTime = readField<float>(gearbox, shiftTimeOffset);
    const bool settled = validGear && engaged == target && std::isfinite(shiftTime) && shiftTime < 0.001f;
    if (state.engagementExpiresAt) {
        const auto engagement = observeEngagement(state.view.requestedGear, engaged, settled, now, state.engagementExpiresAt);
        if (engagement == Engagement::pending)
            return std::nullopt;
        if (engagement == Engagement::confirmed)
            state.view.engagementDelayMs = now - state.view.requestedAt;
        else
            state.view.outcome = "engagement_unconfirmed";
        state.view.requestPending = false;
        state.engagementExpiresAt = 0;
    }
    if (settled && engaged != reverseGear && !state.dispatchExpiresAt)
        state.view.reverseIssued = false;

    // First acquire a current gearbox observation. A held selector can wait within its request deadline.
    if (!state.enabled || !state.request || !observedBeforeStep)
        return std::nullopt;
    const float cooldown = readField<float>(gearbox, shiftCooldownOffset);
    const bool cooldownReady = std::isfinite(cooldown) && cooldown < 0.0f;
    if (!settled || (!cooldownReady && state.request->absolute != engaged))
        return std::nullopt;
    state.dispatchExpiresAt = state.request->expiresAt;
    return ReadyRequest{*std::exchange(state.request, std::nullopt), state.selectionRevision, direction,
                        state.heading.current(owner, now)};
}

bool liveRequest(const void* owner, std::uint64_t generation, const ReadyRequest& command) noexcept {
    return state.enabled && liveLease(owner, generation) &&
           state.selectionRevision == command.selectionRevision &&
           (!command.request.absolute || state.selectedGear == command.request.absolute);
}

enum class FollowUp { none, afterNeutral, engagement };

void recordRequest(const void* owner, std::uint64_t generation, const ReadyRequest& command,
                   int requested, const char* outcome, const void* gearbox,
                   std::optional<int> held = std::nullopt, FollowUp followUp = FollowUp::none) noexcept {
    StateGuard guard;
    if (!liveRequest(owner, generation, command))
        return;
    state.view.requestedGear = requested;
    state.view.engagedGear = readField<int>(gearbox, engagedGearOffset);
    state.view.targetGear = readField<int>(gearbox, targetGearOffset);
    state.view.outcome = outcome;
    state.dispatchExpiresAt = 0;
    if (command.request.absolute && held)
        state.heldGear = held;
    state.view.requestPending = followUp != FollowUp::none;
    if (followUp == FollowUp::afterNeutral)
        state.request = command.request;
    if (followUp == FollowUp::engagement)
        state.engagementExpiresAt = GetTickCount64() + engagementLifetimeMs;
}

void updateGearbox(void* gearbox, float deltaTime) noexcept {
    const auto original = std::bit_cast<Step>(originalStep);
    auto* owner = publishedVehicle.load(std::memory_order_acquire);
    if (!gearbox || !owner || readField<void*>(gearbox, ownerOffset) != owner ||
        readField<void*>(owner, vehicleGearboxOffset) != gearbox) {
        original(gearbox, deltaTime);
        return;
    }

    std::uint64_t generation{};
    bool enabled{};
    bool leased{};
    {
        StateGuard guard;
        generation = state.generation;
        leased = liveLease(owner, generation);
        enabled = state.enabled && freshGearbox(state.view, GetTickCount64());
    }
    if (!leased) {
        original(gearbox, deltaTime);
        return;
    }
    if (!gameForeground()) {
        {
            StateGuard guard;
            if (liveLease(owner, generation)) {
                state.enabled = false;
                resetSelection();
                state.expiresAt = 0;
                publishedVehicle.store(nullptr, std::memory_order_release);
            }
        }
        original(gearbox, deltaTime);
        return;
    }

    {
        // Scope the filter to native requests issued by this exact step and vehicle.
        const StepScope scope(enabled ? ManualStep{gearbox, owner, generation} : ManualStep{});
        original(gearbox, deltaTime);
    }

    const auto engaged = readField<int>(gearbox, engagedGearOffset);
    const auto target = readField<int>(gearbox, targetGearOffset);
    const auto entries = readField<int>(gearbox, gearCountOffset);
    const auto command = takeReadyRequest(owner, generation, engaged, target, entries, gearbox);
    if (!command)
        return;

    const auto& request = command->request;
    const int requested = request.absolute.value_or(sequentialDestination(engaged, request.delta, entries));
    const bool forwardFromNeutral = engaged == neutralGear && forwardGear(requested, entries);
    const auto velocity = forwardFromNeutral ? vehicleVelocity(owner) : std::nullopt;
    const auto motion = velocity && command->forward ? movementDirection(*velocity, *command->forward) : Direction::unknown;
    const auto plan = planShift(engaged, requested, entries, command->lastDirection, motion);
    if (plan == ShiftPlan::unsupported || (!request.absolute && !forwardGear(requested, entries))) {
        recordRequest(owner, generation, *command, requested, "outside_supported_range", gearbox);
        return;
    }
    const bool throughNeutral = plan == ShiftPlan::stoppedThroughNeutral;
    if (throughNeutral || plan == ShiftPlan::stoppedDirect) {
        const auto speed = speedOf(forwardFromNeutral ? velocity : vehicleVelocity(owner));
        if (!shiftSpeedAllowed(requested, speed)) {
            recordRequest(owner, generation, *command, requested, "direction_change_requires_stop", gearbox);
            return;
        }
    }
    if (plan == ShiftPlan::alreadyEngaged) {
        recordRequest(owner, generation, *command, requested, "already_engaged", gearbox, request.absolute);
        return;
    }

    if (!gameForeground())
        return;
    std::uint64_t newReverseIssue{};
    {
        StateGuard guard;
        expireRequests(GetTickCount64());
        if (!liveRequest(owner, generation, *command))
            return;
        // Remember an issued R shift across mode toggles and missing observations.
        if (requested == reverseGear && !throughNeutral) {
            state.directionHistory.reverseRequested(owner);
            const auto issue = ++state.reverseIssueSerial;
            if (!std::exchange(state.view.reverseIssued, true))
                newReverseIssue = issue;
        }
    }

    // For a direction change, wait for the native neutral shift to finish before requesting
    // the opposite gear. A direct reversing request can also be interpreted as neutral by the game.
    const auto shift = std::bit_cast<Shift>(originalShift);
    const bool accepted = shift(gearbox, throughNeutral ? neutralGear : requested, false, false);
    if (!accepted) {
        if (newReverseIssue) {
            StateGuard guard;
            if (state.vehicle == owner && state.reverseIssueSerial == newReverseIssue)
                state.view.reverseIssued = false;
        }
        recordRequest(owner, generation, *command, requested, "native_rejected", gearbox);
    } else if (throughNeutral) {
        recordRequest(owner, generation, *command, requested, "neutral_transition", gearbox, neutralGear, FollowUp::afterNeutral);
    } else {
        recordRequest(owner, generation, *command, requested, "native_accepted", gearbox, request.absolute, FollowUp::engagement);
    }
}

} // namespace

bool attach(RED4ext::v1::PluginHandle plugin, const RED4ext::v1::Sdk* sdk) noexcept {
    if (!sdk || !sdk->hooking || !sdk->hooking->Attach || !sdk->hooking->Detach || stepTarget || shiftTarget)
        return false;
    try {
        if (!supportedGame())
            return false;
    } catch (...) {
        return false;
    }

    const auto module = GetModuleHandleW(nullptr);
    auto* const step = verifiedAddress(module, stepRva, stepBytes);
    auto* const shift = verifiedAddress(module, shiftRva, shiftBytes);
    if (!step || !shift || !verifiedAddress(module, shiftCallRva, shiftCallBytes) ||
        !verifiedAddress(module, gearboxOwnerLinkRva, gearboxOwnerLinkBytes) ||
        !verifiedAddress(module, bodyLinkRva, bodyLinkBytes) ||
        !verifiedAddress(module, bodyVelocityLoadRva, bodyVelocityLoadBytes) ||
        !verifiedAddress(module, shiftCooldownLoadRva, shiftCooldownLoadBytes) ||
        !verifiedAddress(module, neutralRequestRva, neutralRequestBytes) ||
        !verifiedAddress(module, reverseRequestRva, reverseRequestBytes))
        return false;

    hookPlugin = plugin;
    hookSdk = sdk;
    engineRpmAvailable = verifiedAddress(module, engineLinkRva, engineLinkBytes) &&
        verifiedAddress(module, engineRpmStoreRva, engineRpmStoreBytes);
    if (!sdk->hooking->Attach(plugin, shift, std::bit_cast<void*>(&filterAutomatic), &originalShift))
        return false;
    shiftTarget = shift;

    if (!sdk->hooking->Attach(plugin, step, std::bit_cast<void*>(&updateGearbox), &originalStep)) {
        detach();
        return false;
    }
    stepTarget = step;
    installed.store(true, std::memory_order_release);
    return true;
}

void detach() noexcept {
    installed.store(false, std::memory_order_release);
    setDriverVehicle(nullptr);
    publish(nullptr, false, 0);
    if (stepTarget && hookSdk->hooking->Detach(hookPlugin, stepTarget))
        stepTarget = nullptr;
    if (shiftTarget && hookSdk->hooking->Detach(hookPlugin, shiftTarget))
        shiftTarget = nullptr;
}

bool available() noexcept {
    return installed.load(std::memory_order_acquire);
}

void setDriverVehicle(const void* vehicle, std::optional<std::array<float, 3>> forward) noexcept {
    StateGuard guard;
    state.directionHistory.setVehicle(vehicle);
    state.heading = vehicle && forward ? VehicleHeading{vehicle, *forward, GetTickCount64()} : VehicleHeading{};
}

void publish(void* vehicle, bool enabled, int delta, std::optional<int> absolute, bool captureTelemetry) noexcept {
    StateGuard guard;
    if (!available())
        vehicle = nullptr;

    const auto now = GetTickCount64();
    enabled = vehicle && enabled;
    const bool newVehicle = state.vehicle != vehicle || (vehicle && now >= state.expiresAt);
    if (newVehicle || state.enabled != enabled) {
        ++state.generation;
        resetSelection();
        if (newVehicle) {
            const bool reverseIssued = state.vehicle == vehicle && state.view.reverseIssued;
            state.view = {.reverseIssued = reverseIssued};
        }
        state.view.requestedGear = unavailableGear;
        state.view.requestedAt = 0;
        state.view.engagementDelayMs.reset();
        state.view.outcome = enabled ? "manual_ready" : "automatic";
    }

    state.vehicle = vehicle;
    state.enabled = enabled;
    state.expiresAt = vehicle ? now + leaseDurationMs : 0;
    if (state.captureTelemetry && !captureTelemetry) {
        state.view.telemetrySampledAt = 0;
        state.view.engineRpm.reset();
        state.view.speedMetersPerSecond.reset();
    }
    state.captureTelemetry = vehicle && captureTelemetry;
    expireRequests(now);
    if (enabled) {
        if (absolute != state.selectedGear) {
            const bool keepNeutral = absolute && state.heldGear == neutralGear;
            resetSelection();
            if (keepNeutral)
                state.heldGear = neutralGear;
            state.selectedGear = absolute;
            if (absolute)
                queueShift(absolute, 0, now);
        }
        if (!absolute && !state.view.requestPending && (delta == -1 || delta == 1))
            queueShift(std::nullopt, delta, now);
    }
    publishedVehicle.store(vehicle, std::memory_order_release);
}

Snapshot snapshot(void* vehicle) noexcept {
    StateGuard guard;
    const auto now = GetTickCount64();
    if ((vehicle && state.vehicle != vehicle) || !state.vehicle)
        return {};
    if (now >= state.expiresAt)
        return {.reverseIssued = state.view.reverseIssued};

    expireRequests(now);
    auto view = state.view;
    view.manualRequested = state.enabled;
    view.manualActive = state.enabled && freshGearbox(view, now);
    if (!view.telemetrySampledAt || now - view.telemetrySampledAt >= leaseDurationMs) {
        view.telemetrySampledAt = 0;
        view.engineRpm.reset();
        view.speedMetersPerSecond.reset();
    }
    if (!view.sampledAt || now < view.sampledAt || now - view.sampledAt > gearboxObservationLifetimeMs) {
        view.engagedGear = neutralGear;
        view.targetGear = neutralGear;
        view.gearCount = 0;
        view.sampledAt = 0;
    }
    view.selectedGear = state.selectedGear;
    return view;
}

} // namespace manual_backend
