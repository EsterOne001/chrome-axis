#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/simulation.hpp"

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/vehicleBaseObject.hpp>
#include <winver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using chrome_axis::DiagnosticAccessoryState;
using chrome_axis::DiagnosticAxisState;
using chrome_axis::DiagnosticDelivery;
using chrome_axis::DiagnosticFeedbackState;
using chrome_axis::DiagnosticMode;
using Vehicle = RED4ext::vehicle::BaseObject;
using UpdateVehicleInput = void (*)(Vehicle*);

// Layout and address hash identified for this hook in Direct Wheel 3.4.1.
// The pinned 2.5 SDK supplies the named acceleration/deceleration fields.
constexpr std::uint32_t updateVehicleInputHash = 501486464u;
constexpr std::size_t steeringOffset = 0x278;
constexpr std::size_t driveOffset = 0x270;
constexpr std::size_t secondaryDriveOffset = 0x280;
constexpr auto contextLifetime = 350ms;
constexpr auto inputLifetime = 100ms;
constexpr auto configurationPollInterval = 250ms;
constexpr auto physicalPollInterval = 5ms;
constexpr auto simulationPollInterval = 20ms;
constexpr auto idlePollInterval = 25ms;
constexpr auto hudSignalDuration = 2s;
constexpr auto feedbackInterval = 20ms;
constexpr auto motionLifetime = 250ms;
constexpr auto diagnosticInterval = 150ms;
constexpr auto diagnosticIssueDuration = 1s;

// Values returned to the HUD in driving_context.reds.
enum class HudStatus : std::int32_t {
    hidden = 0, loaded = 1, usbWaiting = 2, usbReady = 3, usbSignal = 4, simulation = 5, usbPartial = 6
};

bool usbHudStatus(HudStatus status) noexcept {
    return status == HudStatus::usbReady || status == HudStatus::usbWaiting;
}

struct DrivingContext {
    Vehicle* vehicle{};
    std::uint64_t generation{};
    std::uint64_t diagnosticSession{};
    Clock::time_point refreshed{};
    Clock::time_point vehicleUpdated{};
};

struct VehicleMotion {
    float forwardSpeed{};
    float sideSpeed{};
    float mass{};
    Clock::time_point sampled{};
    float impactSpeed{};
    float impactSide{};
    Clock::time_point impacted{};
};

struct InputFrame {
    chrome_axis::ControlState state{};
    chrome_axis::ControlReader::Readings readings{};
    std::uint64_t generation{};
    Clock::time_point sampled{};
    bool simulated{};
};

struct HudFrame {
    HudStatus status{};
    std::uint64_t generation{};
    Clock::time_point sampled{};
};

struct AppliedAxis {
    float value{};
    std::uint64_t tick{};
};

struct AxisDelivery {
    std::optional<float> reading;
    float requested{};
    DiagnosticDelivery outcome{DiagnosticDelivery::unavailable};
};

struct VehicleDelivery {
    std::array<AxisDelivery, 3> axes{};
    Clock::time_point sampled{};
};

std::mutex stateMutex;
DrivingContext context;
std::array<AppliedAxis, 3> appliedAxes;
VehicleDelivery vehicleDelivery;
VehicleMotion vehicleMotion;
InputFrame inputFrame;
HudFrame hudFrame;
Clock::time_point lastUsbSignal{};
chrome_axis::Simulator simulator;
Clock::time_point lastSimulationStep{};
std::jthread readerThread;
std::atomic<bool> active{};
std::atomic<bool> feedbackRequested{};
std::atomic<Vehicle*> drivingVehicle{};
std::atomic<std::uint64_t> drivingGeneration{};
std::atomic<bool> appliedInput{};
std::atomic<bool> appliedSimulation{};
RED4ext::v1::PluginHandle plugin{};
const RED4ext::v1::Sdk* sdk{};
void* hookTarget{};
void* originalUpdate{};

void info(const char* message) {
    if (sdk && sdk->logger) {
        sdk->logger->Info(plugin, message);
    }
}

void warn(const char* message) {
    if (sdk && sdk->logger) {
        sdk->logger->Warn(plugin, message);
    }
}

void logDirectInput(std::string_view deviceId, std::string_view stage, HRESULT result, bool available) {
    if (!sdk || !sdk->logger) {
        return;
    }
    if (stage.starts_with("force feedback")) {
        if (available) {
            info("Steering wheel force feedback active.");
        } else {
            sdk->logger->WarnF(plugin, "%.*s failed (0x%08lX). USB input stays available.",
                               static_cast<int>(stage.size()), stage.data(), static_cast<unsigned long>(result));
        }
        return;
    }
    if (deviceId.empty()) {
        // The worker recreates the interface on failure. Only log a new error or a recovery.
        thread_local std::optional<HRESULT> previousFailure;
        if (!available && previousFailure != result) {
            sdk->logger->WarnF(plugin, "DirectInput initialization: %.*s failed (0x%08lX). Will retry.",
                               static_cast<int>(stage.size()), stage.data(), static_cast<unsigned long>(result));
            previousFailure = result;
        } else if (available && previousFailure) {
            info("DirectInput initialization recovered.");
            previousFailure.reset();
        }
        return;
    }
    const auto length = static_cast<int>(deviceId.size());
    if (available) {
        sdk->logger->InfoF(plugin, "USB device %.*s: reading input.", length, deviceId.data());
    } else {
        sdk->logger->WarnF(plugin, "USB device %.*s: %.*s did not provide input (0x%08lX).",
                           length, deviceId.data(), static_cast<int>(stage.size()), stage.data(),
                           static_cast<unsigned long>(result));
    }
}

bool hasSupportedExecutable() {
    const auto executable = chrome_axis::modulePath(nullptr);
    DWORD unused{};
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &unused);
    if (size == 0) {
        return false;
    }
    std::vector<std::byte> version(size);
    if (!GetFileVersionInfoW(executable.c_str(), 0, size, version.data())) {
        return false;
    }
    void* data{};
    UINT dataSize{};
    if (!VerQueryValueW(version.data(), L"\\", &data, &dataSize) || dataSize < sizeof(VS_FIXEDFILEINFO)) {
        return false;
    }
    const auto& fixed = *static_cast<const VS_FIXEDFILEINFO*>(data);
    return fixed.dwSignature == 0xFEEF04BD &&
           fixed.dwFileVersionMS == MAKELONG(0, 3) && fixed.dwFileVersionLS == MAKELONG(46235, 88);
}

void* resolveVehicleHook() {
    // The SDK's universal resolver terminates the game on a missing hash.
    // Use the underlying RED4ext resolver so this optional hook can fail closed.
    const auto loader = GetModuleHandleW(L"RED4ext.dll");
    if (!loader) {
        return nullptr;
    }
    using ResolveAddress = std::uintptr_t (*)(std::uint32_t);
    const auto resolve = std::bit_cast<ResolveAddress>(GetProcAddress(loader, "RED4ext_ResolveAddress"));
    if (!resolve) {
        return nullptr;
    }
    auto* address = reinterpret_cast<void*>(resolve(updateVehicleInputHash));
    MEMORY_BASIC_INFORMATION region{};
    if (!address || VirtualQuery(address, &region, sizeof(region)) == 0 || region.State != MEM_COMMIT ||
        region.AllocationBase != GetModuleHandleW(nullptr) || (region.Protect & PAGE_GUARD) != 0) {
        return nullptr;
    }
    constexpr DWORD executableProtection = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                           PAGE_EXECUTE_WRITECOPY;
    return (region.Protect & executableProtection) != 0 ? address : nullptr;
}

HWND foregroundGameWindow() {
    const HWND window = GetForegroundWindow();
    if (!window || IsIconic(window)) {
        return nullptr;
    }
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() ? GetAncestor(window, GA_ROOT) : nullptr;
}

struct ProfileStamp {
    std::uint64_t created{};
    std::uint64_t written{};
    std::uint64_t size{};
    bool operator==(const ProfileStamp&) const = default;
};

std::optional<ProfileStamp> profileStamp(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    const auto combine = [](DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; };
    return ProfileStamp{combine(attributes.ftCreationTime.dwHighDateTime, attributes.ftCreationTime.dwLowDateTime),
                        combine(attributes.ftLastWriteTime.dwHighDateTime, attributes.ftLastWriteTime.dwLowDateTime),
                        combine(attributes.nFileSizeHigh, attributes.nFileSizeLow)};
}

struct WatchedProfile {
    std::optional<ProfileStamp> stamp;
    Clock::time_point retryAt{Clock::time_point::max()};

    bool changed(const std::filesystem::path& path, Clock::time_point now, bool force) {
        const auto current = profileStamp(path);
        const bool needsRead = force || stamp != current || now >= retryAt;
        stamp = current;
        return needsRead;
    }

    void readFinished(bool valid, Clock::time_point now) {
        retryAt = !valid && stamp ? now + 1s : Clock::time_point::max();
    }
};

bool keyDown(int key) noexcept {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool modifierDown() noexcept {
    return keyDown(VK_CONTROL) || keyDown(VK_MENU) || keyDown(VK_SHIFT) ||
           keyDown(VK_LWIN) || keyDown(VK_RWIN);
}

struct OutputContext {
    Vehicle* vehicle{};
    std::uint64_t generation{};
    Clock::time_point sampled{};

    bool current() const noexcept {
        return active.load(std::memory_order_acquire) && drivingVehicle.load(std::memory_order_acquire) == vehicle &&
               drivingGeneration.load(std::memory_order_acquire) == generation && Clock::now() - sampled <= inputLifetime;
    }
};

struct AccessoryDelivery {
    unsigned int key{};
    bool blocked{};
};

class AccessoryKeys {
public:
    bool hasInjectedKeys() const noexcept { return pending_.load(std::memory_order_acquire); }

    AccessoryDelivery apply(const std::bitset<256>& requested, HWND window = nullptr, OutputContext output = {}) noexcept {
        std::scoped_lock lock(mutex_);
        if (requested.any() && (!window || !output.current())) {
            abandon(requested);
            return {0, true};
        }
        lastSample_ = output.sampled;
        blocked_ &= requested;
        AccessoryDelivery delivery{0, blocked_.any()};
        const auto allowed = requested & ~blocked_;
        release(held_ & ~allowed);
        const auto press = allowed & ~held_;
        if (!window || press.none()) {
            return delivery;
        }
        const auto layout = GetKeyboardLayout(GetWindowThreadProcessId(window, nullptr));
        for (std::size_t key = 0; key < press.size(); ++key) {
            if (!press.test(key)) {
                continue;
            }
            auto scan = MapVirtualKeyExW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX, layout);
            // Some layouts return the keypad scan code even for a dedicated arrow.
            if (scan != 0 && key >= VK_LEFT && key <= VK_DOWN) {
                scan = (scan & 0xff) | 0xe000;
            }
            if (scan == 0) {
                delivery.blocked = true;
                continue;
            }
            if (!output.current() || foregroundGameWindow() != window || modifierDown()) {
                abandon(requested);
                delivery.blocked = true;
                return delivery;
            }
            // Avoid taking ownership of a key that the player already holds.
            if (keyDown(static_cast<int>(key))) {
                blocked_.set(key);
                delivery.blocked = true;
                continue;
            }
            if (send(static_cast<WORD>(scan), 0)) {
                delivery.key = static_cast<unsigned int>(key);
                scans_[key] = static_cast<WORD>(scan);
                held_.set(key);
                pending_.store(true, std::memory_order_release);
                if (!loggedPress_) {
                    info("First USB accessory key sent to Windows with the game in the foreground.");
                    loggedPress_ = true;
                }
            } else {
                delivery.blocked = true;
                if (!loggedFailure_) {
                    warn("Windows did not accept a USB accessory key event (SendInput failed).");
                    loggedFailure_ = true;
                }
            }
        }
        return delivery;
    }

    void releaseStale(Clock::time_point now, bool invalidated) noexcept {
        if (!hasInjectedKeys()) {
            return;
        }
        // A stuck DirectInput call owns neither this mutex nor the frame mutex.
        // The game callback can release keys without waiting on the worker.
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && (invalidated || now - lastSample_ > inputLifetime || !foregroundGameWindow())) {
            abandon(held_);
        }
    }

private:
    void abandon(const std::bitset<256>& requested) noexcept {
        lastSample_ = {};
        blocked_ |= requested | held_;
        release(held_);
    }

    void release(std::bitset<256> keys) noexcept {
        if (keys.none()) {
            return;
        }
        for (std::size_t key = 0; key < keys.size(); ++key) {
            if (keys.test(key) && send(scans_[key], KEYEVENTF_KEYUP)) {
                held_.reset(key);
            }
        }
        pending_.store(held_.any(), std::memory_order_release);
    }

    static bool send(WORD scan, DWORD flags) noexcept {
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wScan = scan & 0xff;
        event.ki.dwFlags = flags | KEYEVENTF_SCANCODE;
        if ((scan & 0xff00) == 0xe000) {
            event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        return SendInput(1, &event, sizeof(event)) == 1;
    }

    std::array<WORD, 256> scans_{};
    std::bitset<256> held_;
    std::bitset<256> blocked_;
    Clock::time_point lastSample_{};
    std::mutex mutex_;
    std::atomic<bool> pending_{};
    bool loggedPress_{};
    bool loggedFailure_{};
};

AccessoryKeys accessoryKeys;

// The input worker and the game callback both invalidate frames under stateMutex.
void resetSimulator() noexcept {
    simulator.reset();
    lastSimulationStep = {};
}

void invalidateFrames() noexcept {
    inputFrame = {};
    vehicleDelivery = {};
    hudFrame = {};
    lastUsbSignal = {};
    vehicleMotion = {};
    resetSimulator();
}

// Called under stateMutex. Publish the invalidation before releasing the lock,
// so an older sample cannot send keyboard or wheel output to a new session.
void advanceDrivingGeneration() noexcept {
    drivingGeneration.store(++context.generation, std::memory_order_release);
}

void feedbackEnabled(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, bool* out, std::int64_t) {
    ++frame->code; // ParamEnd
    if (out) {
        *out = feedbackRequested.load(std::memory_order_relaxed);
    }
}

void receiveMotion(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, void*, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    float forward{}, side{}, mass{};
    RED4ext::GetParameter(frame, &vehicle);
    RED4ext::GetParameter(frame, &forward);
    RED4ext::GetParameter(frame, &side);
    RED4ext::GetParameter(frame, &mass);
    ++frame->code;
    if (!vehicle.instance || !feedbackRequested.load(std::memory_order_relaxed) ||
        !std::isfinite(forward) || !std::isfinite(side)) {
        return;
    }
    std::scoped_lock lock(stateMutex);
    if (static_cast<Vehicle*>(vehicle.instance) == context.vehicle) {
        vehicleMotion.forwardSpeed = std::clamp(forward, -150.0f, 150.0f);
        vehicleMotion.sideSpeed = std::clamp(side, -150.0f, 150.0f);
        vehicleMotion.mass = std::isfinite(mass) && mass > 0.0f ? mass : 1500.0f;
        vehicleMotion.sampled = Clock::now();
    }
}

void receiveImpact(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, void*, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    float velocityChange{}, lateral{};
    RED4ext::GetParameter(frame, &vehicle);
    RED4ext::GetParameter(frame, &velocityChange);
    RED4ext::GetParameter(frame, &lateral);
    ++frame->code;
    if (!vehicle.instance || !feedbackRequested.load(std::memory_order_relaxed) || !foregroundGameWindow() ||
        !std::isfinite(velocityChange) || velocityChange < 1.5f || !std::isfinite(lateral)) {
        return;
    }
    const auto now = Clock::now();
    const auto magnitude = std::min(velocityChange, 25.0f);
    std::scoped_lock lock(stateMutex);
    if (static_cast<Vehicle*>(vehicle.instance) != context.vehicle ||
        now - context.refreshed > contextLifetime || now - context.vehicleUpdated > inputLifetime ||
        (now - vehicleMotion.impacted < 75ms && magnitude <= vehicleMotion.impactSpeed)) {
        return;
    }
    vehicleMotion.impactSpeed = magnitude;
    vehicleMotion.impactSide = std::clamp(lateral, -1.0f, 1.0f);
    vehicleMotion.impacted = now;
}

void driverHeartbeat(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, std::int32_t* out, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    RED4ext::GetParameter(frame, &vehicle);
    ++frame->code; // ParamEnd

    const auto now = Clock::now();
    auto* current = active.load(std::memory_order_acquire) ? static_cast<Vehicle*>(vehicle.instance) : nullptr;
    const bool foreground = current && foregroundGameWindow();
    auto status = HudStatus::hidden;
    bool invalidated;
    {
        std::scoped_lock lock(stateMutex);
        invalidated = context.vehicle != current || (current && now - context.refreshed > contextLifetime);
        if (invalidated) {
            advanceDrivingGeneration();
            context.vehicleUpdated = {};
            invalidateFrames();
        }
        if (current && context.vehicle != current) {
            ++context.diagnosticSession;
            appliedAxes = {};
        }
        context.vehicle = current;
        context.refreshed = now;
        drivingVehicle.store(current, std::memory_order_release);
        if (foreground) {
            status = HudStatus::loaded;
            if (hudFrame.generation == context.generation) {
                const bool sampleLive = hudFrame.sampled != Clock::time_point{}
                    && now - hudFrame.sampled <= inputLifetime;
                const bool vehicleLive = context.vehicleUpdated != Clock::time_point{}
                    && now - context.vehicleUpdated <= inputLifetime;
                if (usbHudStatus(hudFrame.status)) {
                    status = HudStatus::usbWaiting;
                    if (sampleLive && vehicleLive) {
                        status = hudFrame.status;
                        if (lastUsbSignal != Clock::time_point{} && now - lastUsbSignal <= hudSignalDuration)
                            status = status == HudStatus::usbReady ? HudStatus::usbSignal : HudStatus::usbPartial;
                    }
                } else if (hudFrame.status == HudStatus::simulation && sampleLive && vehicleLive) {
                    status = HudStatus::simulation;
                }
            }
        }
    }
    accessoryKeys.releaseStale(now, invalidated || !current);
    if (out) {
        *out = static_cast<std::int32_t>(status);
    }
}

void registerNative() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    constexpr auto name = "ChromeAxis_DriverHeartbeat";
    auto* heartbeat = RED4ext::CGlobalFunction::Create(name, name, &driverHeartbeat);
    heartbeat->flags = {.isNative = true, .isStatic = true};
    heartbeat->AddParam("handle:vehicleBaseObject", "vehicle");
    heartbeat->SetReturnType("Int32");
    rtti->RegisterFunction(heartbeat);

    auto* enabled = RED4ext::CGlobalFunction::Create("ChromeAxis_FeedbackEnabled", "ChromeAxis_FeedbackEnabled", &feedbackEnabled);
    enabled->flags = {.isNative = true, .isStatic = true};
    enabled->SetReturnType("Bool");
    rtti->RegisterFunction(enabled);

    auto* motion = RED4ext::CGlobalFunction::Create("ChromeAxis_VehicleMotion", "ChromeAxis_VehicleMotion", &receiveMotion);
    motion->flags = {.isNative = true, .isStatic = true};
    motion->AddParam("handle:vehicleBaseObject", "vehicle");
    motion->AddParam("Float", "forwardSpeed");
    motion->AddParam("Float", "sideSpeed");
    motion->AddParam("Float", "mass");
    rtti->RegisterFunction(motion);

    auto* impact = RED4ext::CGlobalFunction::Create("ChromeAxis_VehicleImpact", "ChromeAxis_VehicleImpact", &receiveImpact);
    impact->flags = {.isNative = true, .isStatic = true};
    impact->AddParam("handle:vehicleBaseObject", "vehicle");
    impact->AddParam("Float", "velocityChange");
    impact->AddParam("Float", "lateral");
    rtti->RegisterFunction(impact);
    info("Driver context native registered.");
}

bool sameProfile(const std::optional<chrome_axis::Profile>& first, const std::optional<chrome_axis::Profile>& second) {
    if (!first || !second) {
        return first.has_value() == second.has_value();
    }
    const auto values = [](const chrome_axis::AxisCalibration& axis) {
        return std::tie(axis.deviceId, axis.axis, axis.neutral, axis.negative, axis.positive, axis.deadzone);
    };
    return values(first->steering) == values(second->steering) &&
           values(first->throttle) == values(second->throttle) &&
           values(first->brake) == values(second->brake) &&
           first->brakeResponse == second->brakeResponse;
}

DiagnosticAxisState diagnosticAxisState(chrome_axis::AxisStatus state, bool sampleExpired) noexcept {
    using chrome_axis::AxisStatus;
    if (state != AxisStatus::unassigned && sampleExpired)
        return DiagnosticAxisState::sampleExpired;
    switch (state) {
    case AxisStatus::unassigned: return DiagnosticAxisState::unassigned;
    case AxisStatus::deviceUnavailable: return DiagnosticAxisState::deviceUnavailable;
    case AxisStatus::axisUnavailable: return DiagnosticAxisState::axisUnavailable;
    case AxisStatus::waitingForNeutral: return DiagnosticAxisState::waitingForNeutral;
    case AxisStatus::armed: return DiagnosticAxisState::armed;
    case AxisStatus::temporarilyUnavailable: return DiagnosticAxisState::temporarilyUnavailable;
    }
    return DiagnosticAxisState::inactive;
}

DiagnosticAccessoryState diagnosticAccessoryState(chrome_axis::AccessoryReadiness readiness, bool ready,
    bool inputAvailable, bool sampleExpired, bool blocked) noexcept {
    if (blocked)
        return DiagnosticAccessoryState::blocked;
    if (sampleExpired)
        return DiagnosticAccessoryState::sampleExpired;
    if (!inputAvailable || readiness.deviceUnavailable)
        return DiagnosticAccessoryState::deviceUnavailable;
    if (readiness.inputUnavailable)
        return DiagnosticAccessoryState::inputUnavailable;
    if (readiness.temporarilyUnavailable)
        return DiagnosticAccessoryState::temporarilyUnavailable;
    return ready ? DiagnosticAccessoryState::ready : DiagnosticAccessoryState::waiting;
}

DiagnosticFeedbackState diagnosticFeedbackState(const chrome_axis::DirectInput* input) noexcept {
    if (!input)
        return DiagnosticFeedbackState::waiting;
    switch (input->wheelForceState()) {
    case chrome_axis::WheelForceState::waiting: return DiagnosticFeedbackState::waiting;
    case chrome_axis::WheelForceState::active: return DiagnosticFeedbackState::active;
    case chrome_axis::WheelForceState::unavailable: return DiagnosticFeedbackState::unavailable;
    }
    return DiagnosticFeedbackState::waiting;
}

void pollInput(std::stop_token stop, const std::filesystem::path& configurationDirectory) {
    const auto executable = chrome_axis::modulePath(nullptr);
    const auto gameRoot = executable.parent_path().parent_path().parent_path();
    std::optional<chrome_axis::DiagnosticPublisher> diagnostics;
    try {
        if (!gameRoot.empty())
            diagnostics.emplace(gameRoot);
    } catch (const std::exception&) {
        warn("Could not initialize the Chrome Axis diagnostics channel.");
    }
    chrome_axis::DiagnosticSnapshot diagnostic;
    Clock::time_point nextDiagnostic{};
    Clock::time_point accessoryBlockedUntil{};
    Clock::time_point nextTimeoutReport{};
    Clock::time_point nextPendingReport{};
    std::uint64_t diagnosticSession{};
    const auto publishDiagnostics = [&](HWND foreground, const DrivingContext& polled) {
        const auto now = Clock::now();
        nextDiagnostic = now + diagnosticInterval;
        if (!diagnostics || !active.load(std::memory_order_acquire))
            return;
        const bool gameForeground = foregroundGameWindow() != nullptr;
        auto snapshot = diagnostic;
        std::uint64_t drivingSession{};
        {
            std::scoped_lock lock(stateMutex);
            // A vehicle transition can invalidate the worker's sample before publishing.
            if (gameForeground && foreground && context.generation != polled.generation)
                return;
            if (!gameForeground) {
                snapshot.mode = DiagnosticMode::gameNotForeground;
            } else if (!foreground) {
                snapshot.mode = DiagnosticMode::waitingForDriver;
            } else {
                drivingSession = context.diagnosticSession;
                if ((snapshot.mode == DiagnosticMode::usb || snapshot.mode == DiagnosticMode::simulation) &&
                    (context.vehicleUpdated == Clock::time_point{} || now - context.vehicleUpdated > inputLifetime)) {
                    snapshot.mode = DiagnosticMode::waitingForVehicleInput;
                }
            }
            if (context.diagnosticSession != diagnosticSession) {
                diagnosticSession = context.diagnosticSession;
                diagnostic.lastAccessoryKey = 0;
                diagnostic.accessorySentAt = 0;
                snapshot.lastAccessoryKey = 0;
                snapshot.accessorySentAt = 0;
            }
            const bool hasDelivery = drivingSession && vehicleDelivery.sampled != Clock::time_point{} &&
                now - vehicleDelivery.sampled <= inputLifetime;
            for (std::size_t i = 0; i < appliedAxes.size(); ++i) {
                auto& axis = snapshot.axes[i];
                axis.value = appliedAxes[i].value;
                axis.appliedAt = appliedAxes[i].tick;
                if (!drivingSession) {
                    axis.state = DiagnosticAxisState::inactive;
                    axis.inputAvailable = 0;
                }
                if (hasDelivery && (axis.state == DiagnosticAxisState::armed ||
                                    axis.state == DiagnosticAxisState::simulated)) {
                    const auto& delivered = vehicleDelivery.axes[i];
                    if (delivered.outcome != DiagnosticDelivery::unavailable) {
                        axis.inputValue = delivered.reading.value_or(0.0f);
                        axis.inputAvailable = delivered.reading.has_value();
                        axis.requestedValue = delivered.requested;
                        axis.requestedAvailable = 1;
                        axis.delivery = delivered.outcome;
                    }
                }
            }
        }
        if (!drivingSession) {
            snapshot.accessories = DiagnosticAccessoryState::inactive;
            snapshot.accessoryArmed = 0;
            if (snapshot.feedback != DiagnosticFeedbackState::disabled)
                snapshot.feedback = DiagnosticFeedbackState::inactive;
        }
        diagnostics->publish(snapshot, drivingSession);
    };
    std::filesystem::path activeDirectory;
    std::optional<chrome_axis::Profile> profile;
    std::optional<chrome_axis::ControlReader> reader;
    std::optional<chrome_axis::AccessoryProfile> accessoryProfile;
    std::optional<chrome_axis::AccessoryReader> accessories;
    WatchedProfile watchedProfile;
    WatchedProfile watchedAccessories;
    WatchedProfile watchedFeedback;
    chrome_axis::FeedbackSettings feedbackSettings;
    chrome_axis::WheelFeedback wheelFeedback;
    Clock::time_point lastFeedbackUpdate{};
    std::string lastFeedbackWarning;
    std::string profileError, accessoryError, lastProfileWarning, lastAccessoryWarning;
    chrome_axis::SimulationReceiver simulation(gameRoot);
    std::unique_ptr<chrome_axis::DirectInput> input;
    HWND window{};
    Clock::time_point retryInitialization{};
    Clock::time_point nextConfigurationPoll{};
    std::uint64_t lastGeneration{};
    bool wasDriving{};
    bool wasSimulated{};
    bool wasUsbEnabled{};
    bool reportedUsbState{};
    bool reportedAccessoriesOnly{};
    bool reportedDriver{};
    std::optional<std::array<chrome_axis::AxisStatus, 3>> lastAxisStatuses;
    Clock::time_point nextAxisLog{};

    while (!stop.stop_requested()) {
        const auto start = Clock::now();
        DrivingContext current;
        {
            std::scoped_lock lock(stateMutex);
            current = context;
        }
        const HWND foreground = current.vehicle && start - current.refreshed <= contextLifetime
            ? foregroundGameWindow() : nullptr;
        if (current.diagnosticSession != diagnosticSession) {
            diagnosticSession = current.diagnosticSession;
            diagnostic.lastAccessoryKey = 0;
            diagnostic.accessorySentAt = 0;
        }
        if (!foreground) {
            feedbackRequested.store(false, std::memory_order_relaxed);
            if (accessoryKeys.hasInjectedKeys()) {
                accessoryKeys.apply({});
            }
            if (wasDriving) {
                if (input) {
                    input->stopWheelForce();
                }
                wheelFeedback.reset();
                lastFeedbackUpdate = {};
                if (reader) {
                    reader->reset();
                }
                if (accessories) {
                    accessories->reset();
                }
                std::scoped_lock lock(stateMutex);
                if (context.generation == current.generation) {
                    invalidateFrames();
                }
            }
            wasDriving = false;
            lastAxisStatuses.reset();
            if (start >= nextDiagnostic && diagnostics) {
                diagnostic.feedback = feedbackSettings.enabled ? DiagnosticFeedbackState::inactive
                                                                : DiagnosticFeedbackState::disabled;
                publishDiagnostics(foreground, current);
            }
            std::this_thread::sleep_until(start + idlePollInterval);
            continue;
        }
        if (!reportedDriver) {
            info("Player driver context detected with the game in the foreground.");
            reportedDriver = true;
        }

        const auto session = simulation.session();
        const bool simulated = session != chrome_axis::SimulationSession::inactive;
        const bool modeChanged = simulated != wasSimulated;
        const bool windowChanged = foreground != window;
        bool profileChanged = false;
        bool accessoryChanged = false;
        bool presetChanged = false;
        bool feedbackChanged = false;
        if (!simulated && (!wasDriving || modeChanged || start >= nextConfigurationPoll)) {
            nextConfigurationPoll = start + configurationPollInterval;
            const bool enabled = chrome_axis::usbEnabled(configurationDirectory);
            auto selected = enabled ? chrome_axis::activePreset(configurationDirectory) : chrome_axis::ProfilePreset{};
            presetChanged = selected.directory != activeDirectory;
            if (presetChanged) {
                activeDirectory = std::move(selected.directory);
                diagnostic.profile.fill(0);
                if (!selected.name.empty()) {
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, selected.name.data(),
                        static_cast<int>(selected.name.size()), diagnostic.profile.data(),
                        static_cast<int>(diagnostic.profile.size() - 1), nullptr, nullptr);
                }
                watchedProfile = {};
                watchedAccessories = {};
                watchedFeedback = {};
                if (!activeDirectory.empty() && sdk && sdk->logger) {
                    const auto path = activeDirectory.u8string();
                    sdk->logger->InfoF(plugin, "Active USB profile directory: %s", reinterpret_cast<const char*>(path.c_str()));
                }
            }
            const bool usable = enabled && !activeDirectory.empty();
            const bool force = !wasDriving || modeChanged || presetChanged || enabled != wasUsbEnabled;
            const auto profilePath = activeDirectory / chrome_axis::profileFilename;
            const auto accessoryPath = activeDirectory / chrome_axis::accessoryFilename;
            const auto feedbackPath = activeDirectory / chrome_axis::feedbackFilename;
            if (!reportedUsbState || enabled != wasUsbEnabled) {
                info(enabled ? "USB requested (Profil/usb.enabled found)."
                             : "USB not requested (Profil/usb.enabled absent or unreadable).");
                reportedUsbState = true;
            }
            if (usable && watchedProfile.changed(profilePath, start, force)) {
                auto updated = chrome_axis::loadProfile(profilePath, profileError);
                watchedProfile.readFinished(updated.has_value(), start);
                profileChanged = !sameProfile(profile, updated);
                if (profileChanged) {
                    profile = std::move(updated);
                    reader.reset();
                    if (profile) {
                        reader.emplace(*profile);
                        info("USB wheel profile loaded. Release the controls to arm.");
                    }
                }
            }
            if (usable && watchedAccessories.changed(accessoryPath, start, force)) {
                accessoryError.clear();
                auto updated = watchedAccessories.stamp
                    ? chrome_axis::loadAccessoryProfile(accessoryPath, accessoryError) : std::nullopt;
                watchedAccessories.readFinished(updated.has_value(), start);
                accessoryChanged = !chrome_axis::sameAccessoryInputProfile(accessoryProfile, updated);
                accessoryProfile = std::move(updated);
                if (accessoryChanged) {
                    accessories.reset();
                    if (accessoryProfile && !accessoryProfile->empty()) {
                        accessories.emplace(*accessoryProfile);
                        info("USB accessory bindings loaded. Release the controls to arm.");
                    }
                }
            }
            if (usable && watchedFeedback.changed(feedbackPath, start, force)) {
                std::string error;
                const auto updated = chrome_axis::loadFeedbackSettings(feedbackPath, error);
                watchedFeedback.readFinished(updated.has_value(), start);
                const auto next = updated.value_or(chrome_axis::FeedbackSettings{});
                feedbackChanged = feedbackSettings != next;
                feedbackSettings = next;
                if (error != lastFeedbackWarning && !error.empty()) {
                    warn(error.c_str());
                }
                lastFeedbackWarning = std::move(error);
            }
            if (!usable) {
                profileChanged = profile.has_value();
                accessoryChanged = accessoryProfile.has_value();
                reader.reset();
                accessories.reset();
                profile.reset();
                accessoryProfile.reset();
                watchedProfile = {};
                watchedAccessories = {};
                watchedFeedback = {};
                feedbackChanged = feedbackSettings.enabled;
                feedbackSettings = {};
                lastFeedbackWarning.clear();
                profileError = enabled ? "The selected USB profile is unavailable." : "";
                accessoryError.clear();
            }
            const std::string_view analogWarning = !watchedProfile.stamp && accessories
                ? std::string_view{} : std::string_view(profileError);
            const bool accessoriesOnly = enabled && accessories && !reader && !watchedProfile.stamp;
            if (accessoriesOnly && !reportedAccessoriesOnly) {
                info("USB accessories only. No analog calibration profile was found.");
            }
            reportedAccessoriesOnly = accessoriesOnly;
            if (analogWarning != lastProfileWarning) {
                lastProfileWarning = analogWarning;
                if (!lastProfileWarning.empty()) {
                    warn(lastProfileWarning.c_str());
                }
            }
            if (accessoryError != lastAccessoryWarning) {
                lastAccessoryWarning = accessoryError;
                if (!lastAccessoryWarning.empty()) {
                    warn(lastAccessoryWarning.c_str());
                }
            }
            wasUsbEnabled = enabled;
        }
        const bool sourceChanged = modeChanged || session == chrome_axis::SimulationSession::restarted || presetChanged;
        if (!wasDriving || lastGeneration != current.generation || sourceChanged || windowChanged ||
            profileChanged || accessoryChanged) {
            accessoryKeys.apply({});
            if (input) {
                input->stopWheelForce();
            }
            wheelFeedback.reset();
            lastFeedbackUpdate = {};
            if (reader) {
                reader->reset();
            }
            if (accessories) {
                accessories->reset();
            }
            std::scoped_lock lock(stateMutex);
            if (context.generation == current.generation) {
                invalidateFrames();
                if (sourceChanged || profileChanged)
                    appliedAxes = {};
            }
            if (sourceChanged || accessoryChanged) {
                diagnostic.lastAccessoryKey = 0;
                diagnostic.accessorySentAt = 0;
                accessoryBlockedUntil = {};
            }
            if (sourceChanged || profileChanged)
                nextTimeoutReport = {};
            nextPendingReport = {};
            lastGeneration = current.generation;
        }
        if (modeChanged || windowChanged || presetChanged || profileChanged || accessoryChanged) {
            input.reset();
            window = foreground;
            retryInitialization = {};
        }
        if (modeChanged) {
            if (simulated) {
                info("Keyboard simulation active. Release the simulation keys to arm.");
            } else {
                info("Keyboard simulation ended.");
            }
        }
        wasDriving = true;
        wasSimulated = simulated;

        chrome_axis::ControlState controls{};
        chrome_axis::ControlReader::Readings readings;
        auto sampledAt = start;
        bool sampleExpired = false;
        std::bitset<256> requestedKeys;
        const bool physical = !simulated && (reader || accessories);
        const bool wantFeedback = physical && feedbackSettings.enabled && profile && !profile->steering.deviceId.empty();
        feedbackRequested.store(wantFeedback, std::memory_order_relaxed);
        if (feedbackChanged) {
            if (input) {
                input->stopWheelForce();
            }
            wheelFeedback.reset();
            lastFeedbackUpdate = {};
            std::scoped_lock lock(stateMutex);
            if (context.generation == current.generation) {
                vehicleMotion = {};
            }
        }
        if (physical) {
            if ((!input || !input->ready()) && start >= retryInitialization) {
                input = std::make_unique<chrome_axis::DirectInput>(window, &logDirectInput);
                if (!input->ready()) {
                    retryInitialization = start + 1s;
                }
            }
            if (input && input->ready()) {
                sampledAt = Clock::now();
                input->beginPoll();
                if (reader) {
                    controls = reader->poll(*input, diagnostics ? &readings : nullptr);
                }
                if (accessories && current.vehicleUpdated != Clock::time_point{} &&
                    sampledAt - current.vehicleUpdated <= inputLifetime) {
                    requestedKeys = accessories->poll(*input);
                } else if (accessories) {
                    accessories->reset();
                }
                sampleExpired = Clock::now() - sampledAt > inputLifetime;
            }
            if (sampleExpired) {
                if (reader) {
                    reader->reset();
                }
                if (accessories) {
                    accessories->reset();
                }
                controls = {};
                readings = {};
                requestedKeys.reset();
            }
        }
        const bool axesReady = reader && std::ranges::all_of(reader->axisStatuses(), [](chrome_axis::AxisStatus status) {
            return status == chrome_axis::AxisStatus::armed || status == chrome_axis::AxisStatus::unassigned;
        });
        const bool profilesValid = (!watchedProfile.stamp || profileError.empty()) && accessoryError.empty();
        auto readiness = physical && accessories ? accessories->readiness() : chrome_axis::AccessoryReadiness{};
        const bool accessoriesReady = accessoryProfile && !accessoryProfile->empty() &&
            readiness.armed == accessoryProfile->size();
        const bool usbReady = physical && profilesValid && (!reader || axesReady) && (!accessories || accessoriesReady);
        const bool usbCanSend = controls.steeringReady || controls.throttleReady || controls.brakeReady ||
            readiness.armed != 0;
        auto hudStatus = HudStatus::loaded;
        if (simulated) {
            hudStatus = HudStatus::simulation;
        } else if (wasUsbEnabled) {
            hudStatus = usbReady ? HudStatus::usbReady : HudStatus::usbWaiting;
        }
        const bool hasRequestedKeys = requestedKeys.any();
        const OutputContext output{current.vehicle, current.generation, sampledAt};
        bool accessoryContextLive = false;
        std::optional<chrome_axis::FeedbackSample> forceSample;
        {
            std::scoped_lock lock(stateMutex);
            // Timestamp from before polling so a stalled driver cannot publish a fresh-looking frame.
            if (context.generation == current.generation) {
                if (hudFrame.status != hudStatus || !usbCanSend) {
                    lastUsbSignal = {};
                }
                hudFrame = {hudStatus, current.generation, sampledAt};
                if (simulated || reader) {
                    inputFrame = {controls, readings, current.generation, sampledAt, simulated};
                }
                if (hasRequestedKeys) {
                    const auto now = Clock::now();
                    accessoryContextLive = context.vehicle && context.vehicleUpdated != Clock::time_point{} &&
                        now - context.refreshed <= contextLifetime && now - context.vehicleUpdated <= inputLifetime;
                }
                if (wantFeedback && controls.steeringReady) {
                    const auto now = Clock::now();
                    if (context.vehicleUpdated != Clock::time_point{} && vehicleMotion.sampled != Clock::time_point{} &&
                        now - context.vehicleUpdated <= inputLifetime && now - context.refreshed <= contextLifetime &&
                        now - sampledAt <= inputLifetime && now - vehicleMotion.sampled <= motionLifetime) {
                        forceSample = chrome_axis::FeedbackSample{controls.values.steering, vehicleMotion.forwardSpeed,
                            vehicleMotion.sideSpeed, vehicleMotion.mass, vehicleMotion.impactSpeed, vehicleMotion.impactSide,
                            std::chrono::duration<float>(now - vehicleMotion.impacted).count()};
                    }
                }
            }
        }
        const bool accessoryBlocked = hasRequestedKeys &&
            (!accessoryContextLive || stop.stop_requested() || modifierDown());
        if (accessoryBlocked) {
            requestedKeys.reset();
            accessories->reset();
            readiness = {};
        }
        AccessoryDelivery delivery;
        if (accessories || accessoryKeys.hasInjectedKeys()) {
            delivery = accessoryKeys.apply(requestedKeys, foreground, output);
            if (delivery.key) {
                std::scoped_lock lock(stateMutex);
                if (context.generation == current.generation) {
                    diagnostic.lastAccessoryKey = delivery.key;
                    diagnostic.accessorySentAt = GetTickCount64();
                    if (usbHudStatus(hudFrame.status))
                        lastUsbSignal = Clock::now();
                }
            }
        }
        if (!forceSample || stop.stop_requested() || !output.current()) {
            if (lastFeedbackUpdate != Clock::time_point{} && input) {
                input->stopWheelForce();
            }
            wheelFeedback.reset();
            lastFeedbackUpdate = {};
        } else if (lastFeedbackUpdate == Clock::time_point{} || start - lastFeedbackUpdate >= feedbackInterval) {
            const float seconds = lastFeedbackUpdate == Clock::time_point{} ? 0.020f
                : std::chrono::duration<float>(start - lastFeedbackUpdate).count();
            lastFeedbackUpdate = start;
            const auto& steering = profile->steering;
            const float direction = steering.positive > steering.neutral ? 1.0f : -1.0f;
            input->setWheelForce(steering.deviceId, steering.axis,
                                 direction * wheelFeedback.update(feedbackSettings, *forceSample, seconds),
                                 [](const void* value) noexcept {
                                     return static_cast<const OutputContext*>(value)->current();
                                 }, &output);
        }
        if (!simulated && reader && input && input->ready()) {
            const auto& statuses = reader->axisStatuses();
            if (start >= nextAxisLog && statuses != lastAxisStatuses && sdk && sdk->logger) {
                const auto label = [](chrome_axis::AxisStatus status) {
                    using chrome_axis::AxisStatus;
                    switch (status) {
                    case AxisStatus::unassigned: return "unassigned";
                    case AxisStatus::deviceUnavailable: return "device unavailable";
                    case AxisStatus::axisUnavailable: return "axis unavailable";
                    case AxisStatus::waitingForNeutral: return "waiting for neutral";
                    case AxisStatus::armed: return "armed";
                    case AxisStatus::temporarilyUnavailable: return "temporarily unavailable";
                    }
                    return "unknown";
                };
                sdk->logger->InfoF(plugin, "USB axes: steering=%s, throttle=%s, brake=%s.",
                                   label(statuses[0]), label(statuses[1]), label(statuses[2]));
                lastAxisStatuses = statuses;
                nextAxisLog = start + 1s;
            }
        } else {
            lastAxisStatuses.reset();
        }
        if (diagnostics) {
            const auto now = Clock::now();
            if (accessoryBlocked || delivery.blocked)
                accessoryBlockedUntil = now + diagnosticIssueDuration;
        }
        // Capture brief interruptions that fall between regular diagnostic snapshots.
        // A slow effect submission does not change whether the earlier input read succeeded.
        const auto diagnosticAt = Clock::now();
        const bool reportPending = diagnostics && physical && diagnosticAt >= nextPendingReport &&
            (readiness.temporarilyUnavailable || (reader &&
                std::ranges::find(reader->axisStatuses(), chrome_axis::AxisStatus::temporarilyUnavailable) !=
                    reader->axisStatuses().end()));
        if (diagnostics && (diagnosticAt >= nextDiagnostic || (sampleExpired && diagnosticAt >= nextTimeoutReport) ||
            reportPending)) {
            if (sampleExpired)
                nextTimeoutReport = diagnosticAt + diagnosticIssueDuration;
            if (reportPending)
                nextPendingReport = diagnosticAt + diagnosticIssueDuration;
            diagnostic.mode = simulated ? DiagnosticMode::simulation : !wasUsbEnabled ? DiagnosticMode::usbDisabled
                : !physical ? DiagnosticMode::profileUnavailable : DiagnosticMode::usb;
            diagnostic.profileLoaded = physical && !activeDirectory.empty();
            for (std::size_t i = 0; i < diagnostic.axes.size(); ++i) {
                auto& axis = diagnostic.axes[i];
                axis.state = simulated ? DiagnosticAxisState::simulated
                    : !wasUsbEnabled ? DiagnosticAxisState::inactive
                    : !reader ? DiagnosticAxisState::awaitingProfile
                    : diagnosticAxisState(reader->axisStatuses()[i], sampleExpired);
                axis.inputValue = readings[i].value_or(0.0f);
                axis.inputAvailable = readings[i].has_value() && !sampleExpired;
            }
            diagnostic.accessoryCount = physical && accessoryProfile
                ? static_cast<std::uint32_t>(accessoryProfile->size()) : 0;
            diagnostic.accessoryArmed = 0;
            diagnostic.accessories = simulated || !wasUsbEnabled ? DiagnosticAccessoryState::inactive
                : !accessoryError.empty() ? DiagnosticAccessoryState::profileError : DiagnosticAccessoryState::unconfigured;
            if (physical && accessories) {
                diagnostic.accessoryArmed = static_cast<std::uint32_t>(readiness.armed);
                diagnostic.accessories = diagnosticAccessoryState(readiness, accessoriesReady, input && input->ready(),
                                                                   sampleExpired, start < accessoryBlockedUntil);
            }
            diagnostic.feedback = simulated && feedbackSettings.enabled ? DiagnosticFeedbackState::inactive
                : !feedbackSettings.enabled ? DiagnosticFeedbackState::disabled
                : !wantFeedback ? DiagnosticFeedbackState::unavailable
                : diagnosticFeedbackState(input.get());
            publishDiagnostics(foreground, current);
        }
        const auto interval = physical ? physicalPollInterval : simulated ? simulationPollInterval : idlePollInterval;
        std::this_thread::sleep_until(start + interval);
    }
    feedbackRequested.store(false, std::memory_order_relaxed);
    accessoryKeys.apply({});
}

float readField(const Vehicle& vehicle, std::size_t offset) {
    float value{};
    std::memcpy(&value, reinterpret_cast<const std::byte*>(&vehicle) + offset, sizeof(value));
    return value;
}

void writeField(Vehicle& vehicle, std::size_t offset, float value) {
    std::memcpy(reinterpret_cast<std::byte*>(&vehicle) + offset, &value, sizeof(value));
}

void updateVehicle(Vehicle* vehicle) {
    std::bit_cast<UpdateVehicleInput>(originalUpdate)(vehicle);
    if (!active.load(std::memory_order_acquire) || drivingVehicle.load(std::memory_order_acquire) != vehicle) {
        return;
    }

    std::unique_lock lock(stateMutex);
    const auto now = Clock::now();
    if (vehicle != context.vehicle || now - context.refreshed > contextLifetime) {
        resetSimulator();
        vehicleDelivery = {};
        return;
    }
    // Accessories can run without an analog profile, but only while the game is updating the vehicle.
    context.vehicleUpdated = now;
    if (inputFrame.generation != context.generation || now - inputFrame.sampled > inputLifetime) {
        resetSimulator();
        vehicleDelivery = {};
        return;
    }
    if (!foregroundGameWindow()) {
        advanceDrivingGeneration();
        context.vehicleUpdated = {};
        invalidateFrames();
        return;
    }

    auto controls = inputFrame.state;
    const bool simulated = inputFrame.simulated;
    if (simulated) {
        // Advance only while the driving hook runs. A paused or stalled game
        // cannot build up a simulated pedal position in the input thread.
        if (now - lastSimulationStep > inputLifetime) {
            resetSimulator();
        }
        const float seconds = lastSimulationStep == Clock::time_point{}
            ? 0.0f
            : std::chrono::duration<float>(now - lastSimulationStep).count();
        lastSimulationStep = now;
        const auto values = simulator.update(chrome_axis::simulationKeys(), seconds);
        controls = {values, values.steering != 0.0f, values.throttle > 0.0f, values.brake > 0.0f};
    }
    if (!controls.steeringReady && !controls.throttleReady && !controls.brakeReady) {
        vehicleDelivery = {};
        return;
    }
    const float gameSteering = readField(*vehicle, steeringOffset);
    const float gameThrottle = vehicle->acceleration;
    const float gameBrake = vehicle->deceleration;
    if (!std::isfinite(gameSteering) || !std::isfinite(gameThrottle) || !std::isfinite(gameBrake)) {
        vehicleDelivery = {};
        return;
    }

    const float steering = std::clamp(controls.values.steering, -1.0f, 1.0f);
    // Invert the quadratic steering response observed by the upstream mod.
    const float compensated = std::copysign(std::sqrt(std::abs(steering)), steering);
    const bool steer = controls.steeringReady && std::abs(compensated) > std::abs(gameSteering);
    const float throttle = controls.throttleReady ? std::max(gameThrottle, controls.values.throttle) : gameThrottle;
    const float brake = controls.brakeReady ? std::max(gameBrake, controls.values.brake) : gameBrake;
    const bool pedals = throttle != gameThrottle || brake != gameBrake;
    const auto delivery = [&](std::size_t axis, float value, bool ready, bool applied) {
        return AxisDelivery{inputFrame.readings[axis], value, !ready ? DiagnosticDelivery::unavailable
            : applied ? DiagnosticDelivery::applied : DiagnosticDelivery::gameKept};
    };
    vehicleDelivery = {{delivery(0, compensated, controls.steeringReady, steer),
                        delivery(1, controls.values.throttle, controls.throttleReady, throttle != gameThrottle),
                        delivery(2, controls.values.brake, controls.brakeReady, brake != gameBrake)}, inputFrame.sampled};
    if (steer) {
        writeField(*vehicle, steeringOffset, compensated);
    }
    if (pedals) {
        vehicle->acceleration = throttle;
        vehicle->deceleration = brake;
        writeField(*vehicle, driveOffset, throttle - brake);
        writeField(*vehicle, secondaryDriveOffset, throttle - brake);
    }
    if (steer || pedals) {
        const auto tick = GetTickCount64();
        if (steer)
            appliedAxes[0] = {compensated, tick};
        if (throttle != gameThrottle)
            appliedAxes[1] = {throttle, tick};
        if (brake != gameBrake)
            appliedAxes[2] = {brake, tick};
        if (!simulated && usbHudStatus(hudFrame.status))
            lastUsbSignal = now;
    }
    lock.unlock();

    if (steer || pedals) {
        auto& logged = simulated ? appliedSimulation : appliedInput;
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            info(simulated ? "First simulated analog input applied to the player's vehicle."
                           : "First analog input applied to the player's vehicle.");
        }
    }
}

void stopPlugin() {
    active.store(false, std::memory_order_release);
    feedbackRequested.store(false, std::memory_order_relaxed);
    drivingVehicle.store(nullptr, std::memory_order_release);
    accessoryKeys.apply({});
    if (readerThread.joinable()) {
        readerThread.request_stop();
        readerThread.join();
    }
    if (hookTarget) {
        if (sdk->hooking->Detach(plugin, hookTarget)) {
            hookTarget = nullptr;
        } else {
            warn("RED4ext did not confirm hook detachment.");
        }
    }
    std::scoped_lock lock(stateMutex);
    context = {};
    appliedAxes = {};
    invalidateFrames();
}

bool startPlugin() {
    if (!hasSupportedExecutable()) {
        warn("Expected Cyberpunk2077.exe file version 3.0.88.46235. Analog input disabled.");
        return false;
    }
    if (!sdk || !sdk->hooking || !sdk->hooking->Attach || !sdk->hooking->Detach || !sdk->scripts ||
        !sdk->scripts->Add) {
        warn("Required RED4ext plugin APIs are unavailable. Analog input disabled.");
        return false;
    }
    const auto dll = chrome_axis::modulePath(plugin);
    const auto configurationDirectory = chrome_axis::profileDirectory(dll);
    if (configurationDirectory.empty()) {
        warn("Could not resolve the plugin directory. Analog input disabled.");
        return false;
    }
    if (sdk->logger) {
        const auto path = configurationDirectory.u8string();
        sdk->logger->InfoF(plugin, "Chrome Axis USB configuration directory: %s",
                           reinterpret_cast<const char*>(path.c_str()));
    }
    const auto directory = dll.parent_path();
    const auto script = directory / L"scripts" / L"driving_context.reds";
    if (!std::filesystem::is_regular_file(script)) {
        warn("Missing scripts/driving_context.reds beside the DLL. Analog input disabled.");
        return false;
    }

    auto* rtti = RED4ext::CRTTISystem::Get();
    void* target = resolveVehicleHook();
    if (!rtti || !target || !sdk->hooking->Attach(plugin, target, std::bit_cast<void*>(&updateVehicle), &originalUpdate)) {
        warn("Vehicle input hook unavailable. Analog input disabled.");
        return false;
    }
    hookTarget = target;

    readerThread = std::jthread([configurationDirectory](std::stop_token stop) {
        try {
            pollInput(stop, configurationDirectory);
        } catch (const std::exception& failure) {
            feedbackRequested.store(false, std::memory_order_relaxed);
            accessoryKeys.apply({});
            warn(failure.what());
            warn("Chrome Axis input reader stopped. Vanilla input remains available.");
            std::scoped_lock lock(stateMutex);
            invalidateFrames();
        }
    });
    if (!sdk->scripts->Add(plugin, script.c_str())) {
        warn("RED4ext could not register the driving context script. Analog input disabled.");
        return false;
    }
    rtti->AddPostRegisterCallback(&registerNative);
    active.store(true, std::memory_order_release);
    info("Vehicle input hook attached for Cyberpunk 2077 2.5. Waiting for the local driver context.");
    return true;
}

} // namespace

RED4EXT_C_EXPORT std::uint32_t RED4EXT_CALL Supports() {
    return RED4EXT_API_VERSION_1;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* result) {
    result->name = L"Chrome Axis";
    result->author = L"Chrome Axis contributors";
    result->version = RED4EXT_V1_SEMVER(CHROME_AXIS_VERSION_MAJOR, CHROME_AXIS_VERSION_MINOR, CHROME_AXIS_VERSION_PATCH);
    result->runtime = RED4EXT_V1_RUNTIME_VERSION_2_50;
    result->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle, RED4ext::v1::EMainReason reason,
                                      const RED4ext::v1::Sdk* runtime) {
    if (reason == RED4ext::v1::EMainReason::Load) {
        plugin = handle;
        sdk = runtime;
        try {
            if (startPlugin()) {
                return true;
            }
        } catch (const std::exception& failure) {
            warn(failure.what());
        }
        stopPlugin();
        return false;
    } else if (reason == RED4ext::v1::EMainReason::Unload) {
        stopPlugin();
    }
    return true;
}
