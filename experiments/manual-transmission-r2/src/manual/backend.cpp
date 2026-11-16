#include "backend.hpp"
#include "shift_request.hpp"

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

// Cyberpunk 2077 2.5, executable 3.0.88.46235. These routines and fields were
// verified against the local x64 game image. Reject other layouts before hooking.
constexpr std::uintptr_t stepRva = 0x6E2E6C;
constexpr std::uintptr_t shiftRva = 0x81DB9C;
constexpr std::uintptr_t shiftCallRva = 0x6E32BA;
constexpr std::uintptr_t planarSpeedRva = 0x6E45F0;
constexpr std::uintptr_t gearboxOwnerLinkRva = 0x81DB1D;
constexpr std::uintptr_t engineRpmStoreRva = 0x6E0318;
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

constexpr auto ownerOffset = 0x0;
constexpr auto vehicleDriveBodyOffset = 0x5F0;
constexpr auto vehicleGearboxOffset = 0xBC8;
constexpr auto vehicleEngineOffset = 0xBD0;
constexpr auto engineRpmOffset = 0x14;
constexpr auto gearCountOffset = 0x198;
constexpr auto shiftTimeOffset = 0x1AC;
constexpr auto targetGearOffset = 0x1C4;
constexpr auto engagedGearOffset = 0x1C8;
constexpr auto leaseDurationMs = 250ULL;

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

struct SharedState {
    void* vehicle{};
    ULONGLONG expiresAt{};
    std::uint64_t generation{};
    int delta{};
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
    if (gearbox && activeStep.gearbox == gearbox && !immediate && !bypassCooldown && gameForeground()) {
        const auto engaged = readField<int>(gearbox, engagedGearOffset);
        const auto entries = readField<int>(gearbox, gearCountOffset);
        if (engaged != target && chrome_axis::isSupportedForwardGear(engaged, entries) &&
            chrome_axis::isSupportedForwardGear(target, entries)) {
            StateGuard guard;
            if (state.enabled && liveLease(activeStep.owner, activeStep.generation)) {
                ++state.view.suppressedAutoShifts;
                return false;
            }
        }
    }
    return original(gearbox, target, immediate, bypassCooldown);
}

int takeRequest(const void* owner, std::uint64_t generation, int engaged, int target, int entries,
                float speedMetersPerSecond, float rpm) noexcept {
    StateGuard guard;
    if (!liveLease(owner, generation))
        return 0;
    state.view.engagedGear = engaged;
    state.view.targetGear = target;
    state.view.gearCount = entries;
    state.view.sampledAt = GetTickCount64();
    state.view.speedMetersPerSecond = speedMetersPerSecond;
    state.view.engineRpm = rpm;
    return state.enabled ? std::exchange(state.delta, 0) : 0;
}

void recordRequest(const void* owner, std::uint64_t generation, int request, const char* outcome,
                   const void* gearbox) noexcept {
    StateGuard guard;
    if (!liveLease(owner, generation) || !state.enabled)
        return;
    ++state.view.shiftSerial;
    state.view.requestedGear = request;
    state.view.engagedGear = readField<int>(gearbox, engagedGearOffset);
    state.view.targetGear = readField<int>(gearbox, targetGearOffset);
    state.view.outcome = outcome;
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
                state.delta = 0;
                state.expiresAt = 0;
                publishedVehicle.store(nullptr, std::memory_order_release);
            }
        }
        original(gearbox, deltaTime);
        return;
    }

    float speed = -1.0f;
    if (auto* driveBody = readField<void*>(owner, vehicleDriveBodyOffset)) {
        const float observedSpeed = planarSpeed(driveBody);
        if (std::isfinite(observedSpeed) && observedSpeed >= 0.0f)
            speed = observedSpeed;
    }
    {
        // Scope the filter to native requests issued by this exact step and vehicle.
        const StepScope scope(enabled ? ManualStep{gearbox, owner, generation} : ManualStep{});
        original(gearbox, deltaTime);
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
    const int request = takeRequest(owner, generation, engaged, target, entries, speed, rpm);
    if (!request)
        return;

    const int requestedGear = chrome_axis::isSupportedForwardGear(engaged, entries) ? engaged + request : -1;
    const char* outcome = "native_rejected";
    if (!chrome_axis::isSupportedForwardGear(engaged, entries) || target != engaged ||
        readField<float>(gearbox, shiftTimeOffset) >= 0.001f) {
        outcome = "shift_in_progress";
    } else if (!chrome_axis::isSupportedForwardGear(requestedGear, entries)) {
        outcome = "outside_forward_range";
    } else {
        bool stillEnabled{};
        {
            StateGuard guard;
            stillEnabled = state.enabled && liveLease(owner, generation);
        }
        if (!stillEnabled || !gameForeground())
            return;
        const auto shift = std::bit_cast<Shift>(originalShift);
        if (shift(gearbox, requestedGear, false, false))
            outcome = "native_accepted";
    }
    recordRequest(owner, generation, requestedGear, outcome, gearbox);
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
        !verifiedAddress(module, engineRpmStoreRva, engineRpmStoreBytes))
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

void publish(void* vehicle, bool enabled, int delta) noexcept {
    StateGuard guard;
    if (!installed.load(std::memory_order_acquire))
        vehicle = nullptr;

    enabled = vehicle && enabled;
    if (state.vehicle != vehicle || state.enabled != enabled || GetTickCount64() >= state.expiresAt) {
        ++state.generation;
        state.delta = 0;
        state.view.engagedGear = -1;
        state.view.targetGear = -1;
        state.view.gearCount = 0;
        state.view.sampledAt = 0;
        state.view.speedMetersPerSecond = -1.0f;
        state.view.engineRpm = -1.0f;
        state.view.requestedGear = -1;
        state.view.outcome = enabled ? "manual_ready" : "automatic";
    }
    state.vehicle = vehicle;
    state.enabled = enabled;
    state.expiresAt = vehicle ? GetTickCount64() + leaseDurationMs : 0;
    if (enabled && state.delta == 0 && (delta == -1 || delta == 1))
        state.delta = delta;
    publishedVehicle.store(vehicle, std::memory_order_release);
}

Snapshot snapshot() noexcept {
    StateGuard guard;
    auto view = state.view;
    if (!state.vehicle || GetTickCount64() >= state.expiresAt) {
        view.engagedGear = -1;
        view.targetGear = -1;
        view.gearCount = 0;
        view.sampledAt = 0;
        view.speedMetersPerSecond = -1.0f;
        view.engineRpm = -1.0f;
    }
    return view;
}

} // namespace manual_backend
