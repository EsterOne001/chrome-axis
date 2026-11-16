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
constexpr std::uintptr_t planarSpeedRva = 0x6E45F0;
constexpr std::uintptr_t gearboxOwnerLinkRva = 0x81DB1D;
constexpr std::uintptr_t engineRpmStoreRva = 0x6E0318;
constexpr std::uintptr_t bodyVelocityLoadRva = 0x6E4641;
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
constexpr std::array<std::uint8_t, 16> planarSpeedBytes{
    0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC1, 0x48, 0x8D, 0x54, 0x24, 0x20,
    0x48, 0x83, 0xC1, 0x20};
constexpr std::array<std::uint8_t, 10> gearboxOwnerLinkBytes{
    0x48, 0x8B, 0x89, 0xC8, 0x0B, 0x00, 0x00, 0x48, 0x89, 0x39};
constexpr std::array<std::uint8_t, 14> engineRpmStoreBytes{
    0xF3, 0x0F, 0x5F, 0x53, 0x68, 0xF3, 0x0F, 0x5D, 0xD0, 0xF3, 0x0F, 0x11, 0x53, 0x14};

constexpr std::array<std::uint8_t, 8> bodyVelocityLoadBytes{
    0x41, 0x0F, 0x10, 0x58, 0x30, 0x0F, 0x28, 0xC3};
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
constexpr float directionChangeSpeed = 0.5f; // m/s

using Step = void (*)(void*, float);
using Shift = bool (*)(void*, int, bool, bool);
using PlanarSpeed = float (*)(void*);

RED4ext::v1::PluginHandle hookPlugin{};
const RED4ext::v1::Sdk* hookSdk{};
void* stepTarget{};
void* shiftTarget{};
void* originalStep{};
void* originalShift{};
PlanarSpeed planarSpeed{};
std::atomic<bool> installed{false};
std::atomic<void*> publishedVehicle{nullptr};
SRWLOCK stateLock = SRWLOCK_INIT;

struct ShiftRequest {
    std::optional<int> absolute;
    int delta{};
    ULONGLONG expiresAt{};
    bool completingDirectionChange{};
};

struct SharedState {
    void* vehicle{};
    ULONGLONG expiresAt{};
    std::uint64_t generation{};
    std::uint64_t selectionRevision{};
    std::optional<int> selectedGear;
    std::optional<int> heldGear;
    std::optional<ShiftRequest> request;
    bool enabled{};
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
            if (!held && state.request && state.request->absolute == neutralGear)
                held = neutralGear;
            if (suppressAutomatic(engaged, target, entries, bypassCooldown, held)) {
                ++state.view.suppressedAutoShifts;
                return false;
            }
        }
    }
    return original(gearbox, target, immediate, bypassCooldown);
}

bool nearStop(const void* driveBody, float planar) noexcept {
    if (!driveBody || !std::isfinite(planar) || planar < 0.0f || planar > directionChangeSpeed)
        return false;
    // The native planar speed routine reads this same velocity vector before projecting it.
    const auto velocity = readField<std::array<float, 3>>(driveBody, bodyVelocityOffset);
    const float absoluteSpeed = std::hypot(velocity[0], velocity[1], velocity[2]);
    return std::isfinite(absoluteSpeed) && absoluteSpeed <= directionChangeSpeed;
}

void resetSelection() noexcept {
    ++state.selectionRevision;
    state.selectedGear.reset();
    state.heldGear.reset();
    state.request.reset();
}

struct ReadyRequest {
    ShiftRequest request;
    std::uint64_t selectionRevision{};
};

std::optional<ReadyRequest> takeReadyRequest(const void* owner, std::uint64_t generation,
                                             int engaged, int target, int entries, float speed, float rpm,
                                             bool settled, bool cooldownReady) noexcept {
    StateGuard guard;
    if (!liveLease(owner, generation))
        return std::nullopt;

    const auto now = GetTickCount64();
    state.view.engagedGear = engaged;
    state.view.targetGear = target;
    state.view.gearCount = entries;
    state.view.sampledAt = now;
    state.view.speedMetersPerSecond = speed;
    state.view.engineRpm = rpm;
    if (!state.enabled || !state.request)
        return std::nullopt;
    if (now >= state.request->expiresAt) {
        state.view.requestedGear = state.request->absolute.value_or(unavailableGear);
        state.view.outcome = "request_expired";
        ++state.view.shiftSerial;
        state.request.reset();
        state.heldGear.reset();
        return std::nullopt;
    }
    if (!settled || (!cooldownReady && state.request->absolute != engaged))
        return std::nullopt;
    return ReadyRequest{*std::exchange(state.request, std::nullopt), state.selectionRevision};
}

bool liveRequest(const void* owner, std::uint64_t generation, const ReadyRequest& command) noexcept {
    return state.enabled && liveLease(owner, generation) &&
           state.selectionRevision == command.selectionRevision &&
           (!command.request.absolute || state.selectedGear == command.request.absolute);
}

void recordRequest(const void* owner, std::uint64_t generation, const ReadyRequest& command,
                   int requested, const char* outcome, const void* gearbox,
                   std::optional<int> held = std::nullopt, bool continueAfterNeutral = false) noexcept {
    StateGuard guard;
    if (!liveRequest(owner, generation, command))
        return;
    ++state.view.shiftSerial;
    state.view.requestedGear = requested;
    state.view.engagedGear = readField<int>(gearbox, engagedGearOffset);
    state.view.targetGear = readField<int>(gearbox, targetGearOffset);
    state.view.outcome = outcome;
    if (command.request.absolute)
        state.heldGear = held;
    if (continueAfterNeutral) {
        state.request = command.request;
        state.request->completingDirectionChange = true;
    }
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
        enabled = state.enabled;
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

    float speed = -1.0f;
    auto* driveBody = readField<void*>(owner, vehicleDriveBodyOffset);
    if (driveBody) {
        const float observedSpeed = planarSpeed(driveBody);
        if (std::isfinite(observedSpeed) && observedSpeed >= 0.0f)
            speed = observedSpeed;
    }
    const auto engaged = readField<int>(gearbox, engagedGearOffset);
    const auto target = readField<int>(gearbox, targetGearOffset);
    const auto entries = readField<int>(gearbox, gearCountOffset);
    float rpm = -1.0f;
    if (const auto* engine = readField<void*>(owner, vehicleEngineOffset)) {
        // The engine integrator clamps its live RPM and stores it at +0x14 (6E0318..6E0325).
        const float observedRpm = readField<float>(engine, engineRpmOffset);
        if (std::isfinite(observedRpm) && observedRpm >= 0.0f)
            rpm = observedRpm;
    }
    const float shiftTime = readField<float>(gearbox, shiftTimeOffset);
    const float cooldown = readField<float>(gearbox, shiftCooldownOffset);
    const bool settled = supportedGear(engaged, entries) && engaged == target &&
                         std::isfinite(shiftTime) && shiftTime < 0.001f;
    const bool cooldownReady = std::isfinite(cooldown) && cooldown < 0.0f;
    const auto command = takeReadyRequest(owner, generation, engaged, target, entries, speed, rpm, settled, cooldownReady);
    if (!command)
        return;

    const auto& request = command->request;
    const int requested = request.absolute.value_or(forwardGear(engaged, entries)
        ? engaged + request.delta : unavailableGear);
    const bool stopped = nearStop(driveBody, speed);
    const auto plan = planShift(engaged, requested, entries, stopped);
    if (plan == ShiftPlan::unsupported || (!request.absolute && !forwardGear(requested, entries))) {
        recordRequest(owner, generation, *command, requested, "outside_supported_range", gearbox);
        return;
    }
    if (plan == ShiftPlan::requiresStop || (request.completingDirectionChange && !stopped)) {
        recordRequest(owner, generation, *command, requested, "direction_change_requires_stop", gearbox);
        return;
    }
    if (plan == ShiftPlan::alreadyEngaged) {
        recordRequest(owner, generation, *command, requested, "already_engaged", gearbox, request.absolute);
        return;
    }

    {
        StateGuard guard;
        if (!liveRequest(owner, generation, *command))
            return;
    }
    if (!gameForeground())
        return;

    const bool throughNeutral = plan == ShiftPlan::throughNeutral;
    // For a direction change, wait for the native neutral shift to finish before requesting
    // the opposite gear. A direct reversing request can also be interpreted as neutral by the game.
    const auto shift = std::bit_cast<Shift>(originalShift);
    const bool accepted = shift(gearbox, throughNeutral ? neutralGear : requested, false, false);
    if (!accepted) {
        recordRequest(owner, generation, *command, requested, "native_rejected", gearbox);
    } else if (throughNeutral) {
        recordRequest(owner, generation, *command, requested, "neutral_transition", gearbox, neutralGear, true);
    } else {
        recordRequest(owner, generation, *command, requested, "native_accepted", gearbox, request.absolute);
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
    auto* const speed = verifiedAddress(module, planarSpeedRva, planarSpeedBytes);
    if (!step || !shift || !speed || !verifiedAddress(module, shiftCallRva, shiftCallBytes) ||
        !verifiedAddress(module, gearboxOwnerLinkRva, gearboxOwnerLinkBytes) ||
        !verifiedAddress(module, engineRpmStoreRva, engineRpmStoreBytes) ||
        !verifiedAddress(module, bodyVelocityLoadRva, bodyVelocityLoadBytes) ||
        !verifiedAddress(module, shiftCooldownLoadRva, shiftCooldownLoadBytes) ||
        !verifiedAddress(module, neutralRequestRva, neutralRequestBytes) ||
        !verifiedAddress(module, reverseRequestRva, reverseRequestBytes))
        return false;

    hookPlugin = plugin;
    hookSdk = sdk;
    planarSpeed = std::bit_cast<PlanarSpeed>(speed);
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
    publish(nullptr, false, 0);
    if (stepTarget && hookSdk->hooking->Detach(hookPlugin, stepTarget))
        stepTarget = nullptr;
    if (shiftTarget && hookSdk->hooking->Detach(hookPlugin, shiftTarget))
        shiftTarget = nullptr;
}

bool available() noexcept {
    return installed.load(std::memory_order_acquire);
}

void publish(void* vehicle, bool enabled, int delta, std::optional<int> absolute) noexcept {
    StateGuard guard;
    if (!available())
        vehicle = nullptr;

    const auto now = GetTickCount64();
    enabled = vehicle && enabled;
    if (state.vehicle != vehicle || state.enabled != enabled || now >= state.expiresAt) {
        ++state.generation;
        resetSelection();
        const auto shiftSerial = state.view.shiftSerial;
        const auto suppressedAutoShifts = state.view.suppressedAutoShifts;
        state.view = {};
        state.view.shiftSerial = shiftSerial;
        state.view.suppressedAutoShifts = suppressedAutoShifts;
        state.view.outcome = enabled ? "manual_ready" : "automatic";
    }

    state.vehicle = vehicle;
    state.enabled = enabled;
    state.expiresAt = vehicle ? now + leaseDurationMs : 0;
    if (enabled) {
        if (absolute != state.selectedGear) {
            resetSelection();
            state.selectedGear = absolute;
            if (absolute)
                state.request = ShiftRequest{absolute, 0, now + requestLifetimeMs};
        }
        if (!absolute && !state.request && (delta == -1 || delta == 1))
            state.request = ShiftRequest{std::nullopt, delta, now + requestLifetimeMs};
    }
    publishedVehicle.store(vehicle, std::memory_order_release);
}

Snapshot snapshot(void* vehicle) noexcept {
    StateGuard guard;
    const auto now = GetTickCount64();
    if ((vehicle && state.vehicle != vehicle) || !state.vehicle || now >= state.expiresAt)
        return {};

    auto view = state.view;
    if (!view.sampledAt || now - view.sampledAt >= leaseDurationMs) {
        view.engagedGear = neutralGear;
        view.targetGear = neutralGear;
        view.gearCount = 0;
        view.sampledAt = 0;
        view.speedMetersPerSecond = -1.0f;
        view.engineRpm = -1.0f;
    }
    view.manualActive = state.enabled;
    view.selectedGear = state.selectedGear;
    return view;
}

} // namespace manual_backend
