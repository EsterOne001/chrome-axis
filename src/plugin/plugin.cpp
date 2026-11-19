#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/simulation.hpp"
#include "accessory_key_polling.hpp"
#include "deferred_driver_cleanup.hpp"
#include "driving_inputs.hpp"
#include "feedback_measurement.hpp"
#include "force_output_worker.hpp"
#include "hud_color_storage.hpp"
#include "manual_transmission_backend.hpp"
#include "manual_transmission_input.hpp"
#include "transmission_hotkey.hpp"
#include "usb_profile_selection.hpp"
#include "watched_profile.hpp"

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
#include <cstdio>
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
using chrome_axis::DiagnosticCommandOutput;
using chrome_axis::DiagnosticCommandState;
using chrome_axis::DiagnosticDelivery;
using chrome_axis::DiagnosticFeedbackState;
using chrome_axis::DiagnosticMode;
using chrome_axis::DiagnosticUsbEvent;
using chrome_axis::DiagnosticUsbEventKind;
using Vehicle = RED4ext::vehicle::BaseObject;
using UpdateVehicleInput = void (*)(Vehicle*);

// Layout and address hash identified for this hook in Direct Wheel 3.4.1.
// The pinned 2.5 SDK supplies the named acceleration/deceleration fields.
constexpr std::uint32_t updateVehicleInputHash = 501486464u;
// Confirmed on the supported 2.5 runtime by the in-game handbrake probe.
constexpr std::size_t handbrakeOffset = 0x26C;
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
constexpr auto motionLifetime = 150ms;
constexpr auto minimumAirTime = 80ms;
constexpr auto diagnosticInterval = 150ms;
constexpr auto captureInterval = 50ms;
constexpr auto diagnosticIssueDuration = 1s;
constexpr auto accessoryPulseDuration = 50ms;

// Values returned to the HUD in driving_context.reds.
enum class HudStatus : std::int32_t {
    hidden = 0, loaded = 1, usbWaiting = 2, usbReady = 3, usbSignal = 4, simulation = 5, usbPartial = 6
};

bool usbHudStatus(HudStatus status) noexcept {
    return status == HudStatus::usbReady || status == HudStatus::usbWaiting || status == HudStatus::usbPartial;
}

struct DrivingContext {
    Vehicle* vehicle{};
    Vehicle* mounted{};
    std::uint64_t generation{};
    std::uint64_t session{};
    Clock::time_point refreshed{};
    Clock::time_point vehicleUpdated{};
    bool menuPaused{};
};

bool vehicleInputFresh(const DrivingContext& driving, Clock::time_point now) noexcept {
    return driving.vehicleUpdated != Clock::time_point{} && now - driving.vehicleUpdated <= inputLifetime;
}

bool sourceInputFresh(Clock::time_point sampled, Clock::time_point now) noexcept {
    return sampled == Clock::time_point{} || now - sampled <= inputLifetime;
}

struct VehicleMotion {
    RED4ext::Vector4 velocity{};
    RED4ext::Vector4 forward{};
    float simTime{};
    float forwardSpeed{};
    float sideSpeed{};
    float mass{};
    float lateralAcceleration{};
    float yawRate{};
    bool skidding{};
    bool airborne{};
    Clock::time_point sampled{};
    Clock::time_point airborneSince{};
    float landingSpeed{};
    Clock::time_point landed{};
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
    std::optional<float> handbrake;
    bool nativeShifter{};
    bool nativeShifterVerified{};
    std::optional<int> nativeGear;
    bool nativeSequential{};
    bool sequentialArmed{};
    std::optional<chrome_axis::ClutchMode> clutchMode;
    std::optional<float> clutch;
    bool selectorAmbiguous{};
    Clock::time_point handbrakeSampledAt{}, gearSampledAt{}, sequentialSampledAt{}, clutchSampledAt{};
    std::optional<chrome_axis::AccessoryObservation> clutchObservation;
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

struct HandbrakeDelivery {
    float requested{};
    float finalValue{};
};

struct VehicleDelivery {
    std::array<AxisDelivery, 3> axes{};
    Clock::time_point sampled{};
    std::array<float, 2> finalPedals{};
    std::uint64_t pedalTick{};
    std::optional<HandbrakeDelivery> handbrake;
    std::optional<float> clutchAttenuation;
};

struct InputAgeCapture {
    Clock::time_point started{};
    chrome_axis::PerformanceTiming timing;
};

std::mutex stateMutex;
DrivingContext context;
std::array<AppliedAxis, 3> appliedAxes;
VehicleDelivery vehicleDelivery;
VehicleMotion vehicleMotion;
InputFrame inputFrame;
InputAgeCapture inputAgeCapture;
HudFrame hudFrame;
Clock::time_point lastUsbSignal{};
chrome_axis::Simulator simulator;
chrome_axis::ManualTransmissionInput transmissionInput;
struct TransmissionShortcutHistory {
    unsigned int key{};
    std::uint64_t pressedAt{};
    std::uint64_t pressCount{};
    chrome_axis::DiagnosticShortcutSource source{};
};
TransmissionShortcutHistory transmissionShortcut;
chrome_axis::ClutchShiftAssist clutchAssist;
chrome_axis::KeyboardClutchProbe keyboardClutch;
std::optional<float> probePosition;
bool clutchShiftBlocked{};
bool probeThrottleInterlock{};
std::array<char, 64> captureVehicleLabel{};
std::atomic<bool> drivingCaptureActive{};
chrome_axis::PaddleRequests paddleRequests;
chrome_axis::TransmissionPedals transmissionPedals;
Clock::time_point lastSimulationStep{};
std::jthread readerThread;
std::atomic<bool> active{};
std::atomic<bool> feedbackRequested{};
std::atomic<DiagnosticMode> feedbackSource{DiagnosticMode::usbDisabled};
std::atomic<DiagnosticMode> usbReaderMode{DiagnosticMode::waitingForDriver};
std::atomic<bool> transmissionAvailable{};
std::atomic<Vehicle*> drivingVehicle{};
std::atomic<std::uint64_t> drivingGeneration{};
std::atomic<bool> appliedInput{};
std::atomic<bool> appliedSimulation{};
RED4ext::v1::PluginHandle plugin{};
const RED4ext::v1::Sdk* sdk{};
std::filesystem::path hudStorageGameRoot;
void* hookTarget{};
void* originalUpdate{};
std::mutex driverLogMutex;
bool driverLoggingEnabled{};

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
    // Workers may finish a driver call after RED4ext has unloaded this plugin.
    std::scoped_lock lock(driverLogMutex);
    if (!driverLoggingEnabled || !sdk || !sdk->logger) {
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
        const auto* message = stage == chrome_axis::usbReaderStartOperation
            ? "USB device %.*s: reader thread restarted; waiting for input."
            : "USB device %.*s: reading input.";
        sdk->logger->InfoF(plugin, message, length, deviceId.data());
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

using chrome_axis::WatchedProfile;

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

struct ForceOutputContext {
    OutputContext input;
    Clock::time_point motionSampled;
    Clock::time_point vehicleUpdated;

    bool current() const noexcept {
        const auto now = Clock::now();
        return input.current() && now <= deadline();
    }

    Clock::time_point deadline() const noexcept {
        return std::min({input.sampled + inputLifetime, motionSampled + motionLifetime, vehicleUpdated + inputLifetime});
    }
};

DiagnosticMode feedbackObservationMode() noexcept {
    if (!foregroundGameWindow())
        return DiagnosticMode::gameNotForeground;
    if (!drivingVehicle.load(std::memory_order_acquire))
        return DiagnosticMode::waitingForDriver;

    // Driver callbacks sample the focus directly. Use the last source
    // configuration and never wait for a game callback to finish.
    const auto source = feedbackSource.load(std::memory_order_acquire);
    std::unique_lock lock(stateMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return source;
    const auto now = Clock::now();
    if (!context.vehicle || now - context.refreshed > contextLifetime)
        return DiagnosticMode::waitingForDriver;
    return chrome_axis::isDrivingCaptureMode(source) && !vehicleInputFresh(context, now)
        ? DiagnosticMode::waitingForVehicleInput : source;
}

DiagnosticMode usbReaderObservationMode() noexcept {
    return usbReaderMode.load(std::memory_order_acquire);
}

struct DriverWorkers {
    chrome_axis::ForceOutputWorker<ForceOutputContext> force{&logDirectInput, &feedbackObservationMode};
    std::unique_ptr<chrome_axis::DrivingInputs<>> input;
};

std::unique_ptr<chrome_axis::DeferredDriverCleanup<DriverWorkers>> driverCleanup;

struct AccessoryDelivery {
    unsigned int key{};
    std::bitset<256> pressed;
    std::bitset<256> held;
    std::bitset<256> pulsing;
    std::bitset<256> blocked;
    std::bitset<256> failed;
};

class AccessoryPresses {
public:
    bool pending() const noexcept { return count_ != 0; }
    void clear() noexcept { count_ = 0; }

    bool push(unsigned int key, Clock::time_point sampled, bool tap) noexcept {
        if (count_ == presses_.size())
            return false;
        presses_[count_++] = {key, sampled, tap};
        return true;
    }

    void keep(std::bitset<256> keys, Clock::time_point now) noexcept {
        const auto end = presses_.begin() + count_;
        count_ = std::remove_if(presses_.begin(), end, [&](const auto& press) {
            return !keys.test(press.key) || !sourceInputFresh(press.sampled, now);
        }) - presses_.begin();
    }

    std::bitset<256> take(std::bitset<256> busy, Clock::time_point now,
                         chrome_axis::AccessoryKeyTimes& times, std::bitset<256>& taps,
                         std::span<unsigned int> order, std::size_t& orderCount) noexcept {
        std::bitset<256> selected;
        std::size_t remaining = 0;
        for (const auto& press : std::span(presses_).first(count_)) {
            if (!sourceInputFresh(press.sampled, now))
                continue;
            if (busy.test(press.key) || selected.test(press.key)) {
                presses_[remaining++] = press;
                continue;
            }
            selected.set(press.key);
            times[press.key] = press.sampled;
            taps.set(press.key, press.tap);
            order[orderCount++] = press.key;
        }
        count_ = remaining;
        return selected;
    }

private:
    struct Press {
        unsigned int key{};
        Clock::time_point sampled{};
        bool tap{};
    };
    std::array<Press, chrome_axis::maxAccessoryBindings * 4> presses_{};
    std::size_t count_{};
};

class AccessoryKeys {
public:
    bool hasInjectedKeys() const noexcept { return pending_.load(std::memory_order_acquire); }
    bool allowsTransmissionKeyPolling(unsigned int key) const noexcept {
        return injectionGuard_.allowsPolling(key);
    }

    AccessoryDelivery apply(const std::bitset<256>& requested, AccessoryPresses* queued = nullptr,
                            HWND window = nullptr, OutputContext output = {},
                            const chrome_axis::AccessoryKeyTimes* keyTimes = nullptr,
                            std::bitset<256> releases = {}) noexcept {
        std::scoped_lock lock(mutex_);
        AccessoryDelivery delivery;
        const auto finish = [&] {
            delivery.held = held_ & ~pulsing_;
            delivery.pulsing = pulsing_;
            return delivery;
        };
        std::bitset<256> taps;
        std::bitset<256> livePulses;
        chrome_axis::AccessoryKeyTimes pressTimes;
        std::array<unsigned int, 256> order;
        std::size_t orderCount{};
        if (!window || !output.current()) {
            if (queued && queued->pending()) {
                livePulses = queued->take({}, Clock::now(), pressTimes, taps, order, orderCount);
                queued->clear();
            }
            abandon(requested);
            delivery.blocked = requested | livePulses;
            return finish();
        }
        lastSample_ = output.sampled;
        blocked_ &= requested & ~releases;
        delivery.blocked = blocked_;
        auto allowed = requested & ~blocked_;
        if (keyTimes) {
            const auto now = Clock::now();
            for (std::size_t key = 0; key < requested.size(); ++key) {
                if (!requested.test(key))
                    continue;
                const auto captured = (*keyTimes)[key];
                if (sourceInputFresh(captured, now)) {
                    if (allowed.test(key) && held_.test(key))
                        keyTimes_[key] = captured;
                } else {
                    allowed.reset(key);
                    delivery.blocked.set(key);
                }
            }
        }
        auto due = held_ & ((~allowed & ~pulsing_) | releases);
        if (pulsing_.any()) {
            const auto now = Clock::now();
            for (std::size_t key = 0; key < pulsing_.size(); ++key) {
                if (pulsing_.test(key) && (now >= pulseUntil_[key] || !sourceInputFresh(keyTimes_[key], now)))
                    due.set(key);
            }
        }
        release(due);
        // Retry a queued pulse only after the previous key-up reaches Windows.
        if (queued && queued->pending())
            livePulses = queued->take(held_ | due, Clock::now(), pressTimes, taps, order, orderCount);
        const auto intents = requested | livePulses;
        const auto holdPresses = allowed & ~held_ & ~due & ~livePulses;
        if (holdPresses.any()) {
            for (unsigned int key = 0; key < holdPresses.size(); ++key) {
                if (holdPresses.test(key))
                    order[orderCount++] = key;
            }
        }
        if (!orderCount)
            return finish();
        const auto layout = GetKeyboardLayout(GetWindowThreadProcessId(window, nullptr));
        for (const auto key : std::span(order).first(orderCount)) {
            auto scan = MapVirtualKeyExW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX, layout);
            // Some layouts return the keypad scan code even for a dedicated arrow.
            if (scan != 0 && key >= VK_LEFT && key <= VK_DOWN) {
                scan = (scan & 0xff) | 0xe000;
            }
            if (scan == 0) {
                delivery.failed.set(key);
                continue;
            }
            if (!output.current() || foregroundGameWindow() != window || modifierDown()) {
                if (queued)
                    queued->clear();
                abandon(requested);
                delivery.blocked |= intents & ~delivery.pressed;
                return finish();
            }
            const auto captured = livePulses.test(key) ? pressTimes[key]
                : keyTimes ? (*keyTimes)[key] : Clock::time_point{};
            if (!sourceInputFresh(captured, Clock::now())) {
                delivery.blocked.set(key);
                continue;
            }
            // Avoid taking ownership of a key that the player already holds.
            if (keyDown(static_cast<int>(key))) {
                if (requested.test(key))
                    blocked_.set(key);
                delivery.blocked.set(key);
                continue;
            }
            injectionGuard_.beforeSend(key, scan, layout);
            if (send(static_cast<WORD>(scan), 0)) {
                delivery.key = static_cast<unsigned int>(key);
                delivery.pressed.set(key);
                scans_[key] = static_cast<WORD>(scan);
                held_.set(key);
                keyTimes_[key] = captured;
                if (livePulses.test(key) && !taps.test(key)) {
                    pulsing_.set(key);
                    pulseUntil_[key] = Clock::now() + accessoryPulseDuration;
                }
                pending_.store(true, std::memory_order_release);
                if (!loggedPress_) {
                    info("First USB accessory key sent to Windows with the game in the foreground.");
                    loggedPress_ = true;
                }
                if (taps.test(key)) {
                    std::bitset<256> released;
                    released.set(key);
                    release(released);
                }
            } else {
                delivery.failed.set(key);
                if (!loggedFailure_) {
                    warn("Windows did not accept a USB accessory key event (SendInput failed).");
                    loggedFailure_ = true;
                }
            }
        }
        return finish();
    }

    void releaseStale(Clock::time_point now, bool invalidated) noexcept {
        if (!hasInjectedKeys()) {
            return;
        }
        // A stuck DirectInput call owns neither this mutex nor the frame mutex.
        // The game callback can release keys without waiting on the worker.
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        if (invalidated || now - lastSample_ > inputLifetime || !foregroundGameWindow()) {
            abandon(held_);
            return;
        }
        std::bitset<256> expired;
        for (std::size_t key = 0; key < held_.size(); ++key) {
            if (held_.test(key) && (!sourceInputFresh(keyTimes_[key], now) ||
                (pulsing_.test(key) && now >= pulseUntil_[key])))
                expired.set(key);
        }
        blocked_ |= expired & ~pulsing_;
        release(expired);
    }

    void releaseBindings(std::bitset<256> keys) noexcept {
        std::scoped_lock lock(mutex_);
        release(held_ & keys);
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
                pulsing_.reset(key);
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
    std::bitset<256> pulsing_;
    std::array<Clock::time_point, 256> pulseUntil_{};
    chrome_axis::AccessoryKeyTimes keyTimes_{};
    std::bitset<256> blocked_;
    Clock::time_point lastSample_{};
    std::mutex mutex_;
    std::atomic<bool> pending_{};
    chrome_axis::AccessoryKeyPollingGuard injectionGuard_;
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
    paddleRequests.clear();
    keyboardClutch = {};
    probePosition.reset();
    clutchAssist = {};
    clutchShiftBlocked = false;
    vehicleDelivery = {};
    hudFrame = {};
    lastUsbSignal = {};
    vehicleMotion = {};
    resetSimulator();
}

// Called under stateMutex. Publish the invalidation before releasing the lock,
// so an older sample cannot send keyboard or wheel output to a new session.
void advanceDrivingGeneration() noexcept {
    chrome_axis::setTransmissionHotkeyContext(false);
    drivingGeneration.store(++context.generation, std::memory_order_release);
}

void feedbackEnabled(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, bool* out, std::int64_t) {
    ++frame->code; // ParamEnd
    if (out) {
        *out = feedbackRequested.load(std::memory_order_relaxed);
    }
}

bool finiteSpatial(const RED4ext::Vector4& vector) noexcept {
    return std::isfinite(vector.X) && std::isfinite(vector.Y) && std::isfinite(vector.Z);
}

float dotSpatial(const RED4ext::Vector4& first, const RED4ext::Vector4& second) noexcept {
    return first.X * second.X + first.Y * second.Y + first.Z * second.Z;
}

void receiveMotion(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, void*, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    RED4ext::Vector4 velocity, forward, right;
    float mass{}, simTime{};
    bool skidding{}, airborne{};
    RED4ext::GetParameter(frame, &vehicle);
    RED4ext::GetParameter(frame, &velocity);
    RED4ext::GetParameter(frame, &forward);
    RED4ext::GetParameter(frame, &right);
    RED4ext::GetParameter(frame, &mass);
    RED4ext::GetParameter(frame, &simTime);
    RED4ext::GetParameter(frame, &skidding);
    RED4ext::GetParameter(frame, &airborne);
    ++frame->code;
    if (!vehicle.instance || !feedbackRequested.load(std::memory_order_relaxed) ||
        !foregroundGameWindow() || !finiteSpatial(velocity) || !finiteSpatial(forward) ||
        !finiteSpatial(right) || !std::isfinite(simTime) || dotSpatial(velocity, velocity) > 22500.0f) {
        return;
    }
    std::scoped_lock lock(stateMutex);
    const auto now = Clock::now();
    if (static_cast<Vehicle*>(vehicle.instance) != context.vehicle || now - context.refreshed > contextLifetime ||
        context.vehicleUpdated == Clock::time_point{} || now - context.vehicleUpdated > inputLifetime) {
        return;
    }

    auto& motion = vehicleMotion;
    const float seconds = simTime - motion.simTime;
    // A duplicate (or nearly duplicate) simulation tick is not a newer measurement.
    if (motion.sampled != Clock::time_point{} && seconds >= 0.f && seconds < 0.005f)
        return;
    const bool continuous = motion.sampled != Clock::time_point{} && now - motion.sampled <= motionLifetime &&
        seconds >= 0.005f && seconds <= 0.16f;
    // Velocity differences must be taken in world space before projection onto the car's axes.
    const auto deltaVelocity = velocity - motion.velocity;
    if (continuous && dotSpatial(deltaVelocity, deltaVelocity) <= 400.0f) {
        const float blend = std::min(seconds / 0.12f, 1.0f);
        const float lateralAcceleration = std::clamp(dotSpatial(deltaVelocity, right) / seconds, -25.0f, 25.0f);
        motion.lateralAcceleration = std::lerp(motion.lateralAcceleration, lateralAcceleration, blend);

        const float side = forward.X * right.Y - forward.Y * right.X;
        const float crossing = motion.forward.X * forward.Y - motion.forward.Y * forward.X;
        const float alignment = motion.forward.X * forward.X + motion.forward.Y * forward.Y;
        if (std::abs(side) > 0.1f && crossing * crossing + alignment * alignment > 0.01f) {
            const float yaw = (side > 0.f ? 1.f : -1.f) * std::atan2(crossing, alignment) / seconds;
            motion.yawRate = std::abs(yaw) <= 6.f ? std::lerp(motion.yawRate, yaw, blend) : 0.f;
        } else {
            motion.yawRate = 0.f;
        }
    } else {
        motion.lateralAcceleration = 0.f;
        motion.yawRate = 0.f;
    }

    if (airborne) {
        if (!motion.airborne || !continuous) {
            motion.airborneSince = now;
            motion.landed = {};
            motion.landingSpeed = 0.f;
        }
    } else if (continuous && motion.airborne && now - motion.airborneSince >= minimumAirTime && motion.velocity.Z < -1.5f) {
        motion.landed = now;
        motion.landingSpeed = std::min(-motion.velocity.Z, 20.f);
    }
    motion.velocity = velocity;
    motion.forward = forward;
    motion.simTime = simTime;
    motion.forwardSpeed = std::clamp(dotSpatial(velocity, forward), -150.f, 150.f);
    motion.sideSpeed = std::clamp(dotSpatial(velocity, right), -150.f, 150.f);
    motion.mass = std::isfinite(mass) && mass > 0.f ? mass : 1500.f;
    motion.skidding = skidding;
    motion.airborne = airborne;
    motion.sampled = now;
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

chrome_axis::DirectGearKeys keyboardGear() noexcept {
    constexpr std::array keys{static_cast<int>('N'), static_cast<int>('R'),
                              static_cast<int>('1'), static_cast<int>('2'), static_cast<int>('3'),
                              static_cast<int>('4'), static_cast<int>('5'), static_cast<int>('6'),
                              static_cast<int>('7')};
    std::optional<int> gear;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keyDown(keys[i])) {
            if (gear)
                return {.gear = std::nullopt, .ambiguous = true};
            gear = static_cast<int>(i) - 1;
        }
    }
    return {.gear = gear};
}

chrome_axis::PedalGear pedalGear(const manual_backend::Snapshot& gearbox) noexcept {
    return chrome_axis::observePedalGear(gearbox.engagedGear, gearbox.targetGear, gearbox.gearCount,
        gearbox.reverseIssued, gearbox.requestPending ? std::optional{gearbox.requestedGear} : std::nullopt);
}

// Caller holds stateMutex. A stale USB frame cannot request a gear in a new vehicle or session.
void suspendTransmission(Vehicle* captureVehicle = nullptr) noexcept {
    transmissionInput.suspend();
    paddleRequests.clear();
    keyboardClutch = {};
    probePosition.reset();
    clutchAssist = {};
    clutchShiftBlocked = false;
    if (transmissionAvailable) {
        if (context.vehicle) {
            const auto gearbox = manual_backend::snapshot(context.vehicle);
            const auto gear = pedalGear(gearbox);
            if (gear == chrome_axis::PedalGear::reverse || gear == chrome_axis::PedalGear::reversePending)
                transmissionPedals.update(gearbox.manualRequested || gearbox.reverseIssued, gear, {});
        }
        manual_backend::publish(captureVehicle, false, 0, std::nullopt, captureVehicle != nullptr);
    }
    transmissionPedals.update(false, chrome_axis::PedalGear::unavailable, {});
}

chrome_axis::PedalRoutingDecision updateTransmission(Vehicle* vehicle, chrome_axis::PedalRelease pedalsReleased,
                                                     unsigned int hotkey, Clock::time_point now,
                                                     bool captureTelemetry) noexcept {
    if (!transmissionAvailable)
        return {};

    const bool simulated = inputFrame.simulated;
    const bool hardware = inputFrame.nativeShifterVerified;
    const bool sequential = inputFrame.nativeSequential;
    const auto gear = sourceInputFresh(inputFrame.gearSampledAt, now) ? inputFrame.nativeGear : std::nullopt;
    const bool sequentialArmed = inputFrame.sequentialArmed && sourceInputFresh(inputFrame.sequentialSampledAt, now);
    const auto clutch = sourceInputFresh(inputFrame.clutchSampledAt, now) ? inputFrame.clutch : std::nullopt;
    paddleRequests.expire(now);
    const bool sendPaddle = sequentialArmed && paddleRequests.pending() && !manual_backend::snapshot(vehicle).requestPending;
    unsigned int physicalPresses = 0;
    const bool hasShortcut = hardware || sequential || simulated;
    chrome_axis::setTransmissionHotkeyContext(hasShortcut);
    if (hasShortcut) {
        if (accessoryKeys.allowsTransmissionKeyPolling(hotkey)) {
            const auto revision = chrome_axis::transmissionHotkeySampleRevision();
            const bool pressed = keyDown(static_cast<int>(hotkey));
            if (accessoryKeys.allowsTransmissionKeyPolling(hotkey))
                chrome_axis::sampleTransmissionHotkey(hotkey, pressed, revision);
        }
        while (physicalPresses < chrome_axis::PhysicalModeKey::pressCapacity) {
            const auto press = chrome_axis::takeTransmissionHotkeyPress(hotkey);
            if (!press)
                break;
            ++physicalPresses;
            transmissionShortcut.key = hotkey;
            transmissionShortcut.pressedAt = press->timestamp;
            ++transmissionShortcut.pressCount;
            transmissionShortcut.source = press->source == chrome_axis::TransmissionHotkeySource::hook
                ? chrome_axis::DiagnosticShortcutSource::hook : chrome_axis::DiagnosticShortcutSource::poll;
        }
    }
    auto intent = transmissionInput.update(hardware, gear, simulated,
        simulated && keyDown(VK_PRIOR), simulated && keyDown(VK_NEXT),
        simulated ? keyboardGear() : chrome_axis::DirectGearKeys{}, sequential, sequentialArmed,
        sendPaddle ? paddleRequests.pop() : 0, physicalPresses);
    if (!intent.manual || !sequentialArmed)
        paddleRequests.clear();

    if (intent.manual && inputFrame.clutchMode == chrome_axis::ClutchMode::assist) {
        if (!sequential || intent.delta)
            clutchShiftBlocked = clutchAssist.apply(clutch, intent.gear, intent.delta);
    } else {
        clutchAssist = {};
        clutchShiftBlocked = false;
    }
    if (simulated && intent.manual) {
        constexpr std::array keys{VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD3, VK_NUMPAD5, VK_NUMPAD7};
        std::array<bool, keys.size()> down{};
        for (std::size_t i = 0; i < keys.size(); ++i)
            down[i] = hotkey != static_cast<unsigned int>(keys[i]) && keyDown(keys[i]);
        probePosition = keyboardClutch.update(down);
    } else {
        keyboardClutch = {};
        probePosition = intent.manual && inputFrame.clutchMode == chrome_axis::ClutchMode::experiment
            ? clutch : std::nullopt;
    }
    manual_backend::publish(vehicle, intent.manual, intent.delta,
                            intent.manual ? intent.gear : std::nullopt, captureTelemetry);
    if (simulated && intent.toggledByKey)
        info(intent.manual ? "Keyboard simulation transmission: MANUAL requested." : "Keyboard simulation transmission: AUTO.");
    if (!intent.manual && !transmissionPedals.tracking())
        return transmissionPedals.update(false, chrome_axis::PedalGear::unavailable, pedalsReleased);

    const auto gearbox = manual_backend::snapshot(vehicle);
    return transmissionPedals.update(intent.manual || gearbox.reverseIssued, pedalGear(gearbox), pedalsReleased);
}

void driverHeartbeat(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, std::int32_t* out, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    bool canDrive{};
    RED4ext::Vector4 forward{};
    RED4ext::GetParameter(frame, &vehicle);
    RED4ext::GetParameter(frame, &canDrive);
    RED4ext::GetParameter(frame, &forward);
    ++frame->code; // ParamEnd

    const auto now = Clock::now();
    auto* mounted = active.load(std::memory_order_acquire) ? static_cast<Vehicle*>(vehicle.instance) : nullptr;
    auto* current = canDrive ? mounted : nullptr;
    const bool foreground = current && foregroundGameWindow();
    auto status = HudStatus::hidden;
    bool invalidated;
    {
        std::scoped_lock lock(stateMutex);
        if (transmissionAvailable) {
            const auto heading = foreground ? std::optional{std::array{forward.X, forward.Y, forward.Z}} : std::nullopt;
            if (mounted)
                manual_backend::setDriverVehicle(vehicle, heading);
            else
                manual_backend::setDriverVehicle(nullptr);
        }
        const bool mountedChanged = context.mounted != mounted;
        invalidated = mountedChanged || context.vehicle != current || (current && now - context.refreshed > contextLifetime);
        if (mountedChanged) {
            transmissionInput = {};
            transmissionShortcut = {};
            transmissionPedals = {};
        }
        if (invalidated) {
            advanceDrivingGeneration();
            context.vehicleUpdated = {};
            invalidateFrames();
        }
        if (invalidated) {
            captureVehicleLabel = {};
            probeThrottleInterlock = false;
        }
        if (current && invalidated) {
            ++context.session;
            appliedAxes = {};
        }
        context.vehicle = current;
        context.mounted = mounted;
        context.refreshed = now;
        context.menuPaused = mounted && !canDrive;
        drivingVehicle.store(current, std::memory_order_release);
        if (invalidated || !foreground) {
            chrome_axis::setTransmissionHotkeyContext(false);
            suspendTransmission();
        }
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
                        if (status == HudStatus::usbReady && lastUsbSignal != Clock::time_point{} &&
                            now - lastUsbSignal <= hudSignalDuration)
                            status = HudStatus::usbSignal;
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

void captureEnabled(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, bool* out, std::int64_t) {
    ++frame->code;
    if (out)
        *out = drivingCaptureActive.load(std::memory_order_relaxed);
}

void captureVehicleRecord(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, void*, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    RED4ext::TweakDBID record;
    RED4ext::GetParameter(frame, &vehicle);
    RED4ext::GetParameter(frame, &record);
    ++frame->code;
    std::scoped_lock lock(stateMutex);
    if (vehicle.instance != context.vehicle || !drivingCaptureActive.load(std::memory_order_relaxed))
        return;
    const auto id = record.value & 0xFFFFFFFFFFull;
    if (id && !captureVehicleLabel.front())
        std::snprintf(captureVehicleLabel.data(), captureVehicleLabel.size(), "TweakDBID:%010llX",
                      static_cast<unsigned long long>(id));
}

void transmissionForHud(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, std::int32_t* out, std::int64_t) {
    RED4ext::Handle<RED4ext::ISerializable> vehicle;
    RED4ext::GetParameter(frame, &vehicle);
    ++frame->code;
    if (out) {
        const auto state = vehicle.instance && transmissionAvailable ? manual_backend::snapshot(vehicle.instance)
                                                                     : manual_backend::Snapshot{};
        const int gear = state.gearCount > 0 && state.engagedGear >= -1 && state.engagedGear < state.gearCount
            ? state.engagedGear : -2;
        // Low bits encode the gear (+2), bit 4 selects MANUAL. Return one coherent sample.
        *out = (gear + 2) | (state.manualActive ? 16 : 0);
    }
}

void commitHudColors(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, std::int32_t* out, std::int64_t) {
    RED4ext::CString json, previousJson;
    bool existed{};
    RED4ext::GetParameter(frame, &json);
    RED4ext::GetParameter(frame, &previousJson);
    RED4ext::GetParameter(frame, &existed);
    ++frame->code;
    if (!out)
        return;
    try {
        *out = static_cast<std::int32_t>(chrome_axis::commitHudColors(
            hudStorageGameRoot, {json.c_str(), json.Length()}, {previousJson.c_str(), previousJson.Length()}, existed));
    } catch (const std::exception&) {
        *out = static_cast<std::int32_t>(chrome_axis::HudColorCommitResult::unavailable);
    }
}

void recoverHudColors(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, std::int32_t* out, std::int64_t) {
    bool probeReadable{};
    RED4ext::GetParameter(frame, &probeReadable);
    ++frame->code;
    if (!out)
        return;
    try {
        *out = static_cast<std::int32_t>(chrome_axis::recoverHudColors(hudStorageGameRoot, probeReadable));
    } catch (const std::exception&) {
        *out = static_cast<std::int32_t>(chrome_axis::HudColorCommitResult::unavailable);
    }
}

void registerNative() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    constexpr auto name = "ChromeAxis_DriverHeartbeat";
    auto* heartbeat = RED4ext::CGlobalFunction::Create(name, name, &driverHeartbeat);
    heartbeat->flags = {.isNative = true, .isStatic = true};
    heartbeat->AddParam("handle:vehicleBaseObject", "vehicle");
    heartbeat->AddParam("Bool", "canDrive");
    heartbeat->AddParam("Vector4", "forward");
    heartbeat->SetReturnType("Int32");
    rtti->RegisterFunction(heartbeat);

    constexpr auto captureEnabledName = "ChromeAxis_CaptureEnabled";
    auto* capture = RED4ext::CGlobalFunction::Create(captureEnabledName, captureEnabledName, &captureEnabled);
    capture->flags = {.isNative = true, .isStatic = true};
    capture->SetReturnType("Bool");
    rtti->RegisterFunction(capture);

    constexpr auto captureRecordName = "ChromeAxis_CaptureVehicleRecord";
    auto* captureRecord = RED4ext::CGlobalFunction::Create(captureRecordName, captureRecordName, &captureVehicleRecord);
    captureRecord->flags = {.isNative = true, .isStatic = true};
    captureRecord->AddParam("handle:vehicleBaseObject", "vehicle");
    captureRecord->AddParam("TweakDBID", "record");
    rtti->RegisterFunction(captureRecord);

    constexpr auto transmissionName = "ChromeAxis_TransmissionState";
    auto* transmission = RED4ext::CGlobalFunction::Create(transmissionName, transmissionName, &transmissionForHud);
    transmission->flags = {.isNative = true, .isStatic = true};
    transmission->AddParam("handle:vehicleBaseObject", "vehicle");
    transmission->SetReturnType("Int32");
    rtti->RegisterFunction(transmission);

    auto* enabled = RED4ext::CGlobalFunction::Create("ChromeAxis_FeedbackEnabled", "ChromeAxis_FeedbackEnabled", &feedbackEnabled);
    enabled->flags = {.isNative = true, .isStatic = true};
    enabled->SetReturnType("Bool");
    rtti->RegisterFunction(enabled);

    auto* motion = RED4ext::CGlobalFunction::Create("ChromeAxis_FeedbackMotion", "ChromeAxis_FeedbackMotion", &receiveMotion);
    motion->flags = {.isNative = true, .isStatic = true};
    motion->AddParam("handle:vehicleBaseObject", "vehicle");
    motion->AddParam("Vector4", "velocity");
    motion->AddParam("Vector4", "forward");
    motion->AddParam("Vector4", "right");
    motion->AddParam("Float", "mass");
    motion->AddParam("Float", "simTime");
    motion->AddParam("Bool", "skidding");
    motion->AddParam("Bool", "airborne");
    rtti->RegisterFunction(motion);

    auto* impact = RED4ext::CGlobalFunction::Create("ChromeAxis_VehicleImpact", "ChromeAxis_VehicleImpact", &receiveImpact);
    impact->flags = {.isNative = true, .isStatic = true};
    impact->AddParam("handle:vehicleBaseObject", "vehicle");
    impact->AddParam("Float", "velocityChange");
    impact->AddParam("Float", "lateral");
    rtti->RegisterFunction(impact);

    constexpr auto colorsName = "ChromeAxis_CommitHudColors";
    auto* colors = RED4ext::CGlobalFunction::Create(colorsName, colorsName, &commitHudColors);
    colors->flags = {.isNative = true, .isStatic = true};
    colors->AddParam("String", "json");
    colors->AddParam("String", "previousJson");
    colors->AddParam("Bool", "existed");
    colors->SetReturnType("Int32");
    rtti->RegisterFunction(colors);

    constexpr auto recoverColorsName = "ChromeAxis_RecoverHudColors";
    auto* recoverColors = RED4ext::CGlobalFunction::Create(recoverColorsName, recoverColorsName, &recoverHudColors);
    recoverColors->flags = {.isNative = true, .isStatic = true};
    recoverColors->AddParam("Bool", "probeReadable");
    recoverColors->SetReturnType("Int32");
    rtti->RegisterFunction(recoverColors);
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
           first->brakeResponse == second->brakeResponse &&
           first->steeringResponse == second->steeringResponse &&
           first->throttleResponse == second->throttleResponse;
}

struct UsbConfigurationChanges {
    bool preset{};
    bool calibration{};
    bool accessories{};
    bool feedback{};
};

// Owned by the polling thread. Reload settings and report what changed so that
// the caller can invalidate frames and release keyboard and force output.
class UsbConfiguration {
public:
    UsbConfigurationChanges refresh(const std::filesystem::path& root, Clock::time_point now, bool force,
                                    chrome_axis::DiagnosticSnapshot& diagnostic);
    void resetReaders() noexcept {
        if (reader)
            reader->reset();
        if (accessories)
            accessories->reset();
    }

    bool hasProfileDirectory() const noexcept { return !directory_.empty(); }
    bool hasAccessoryError() const noexcept { return !accessoryError_.empty(); }
    bool profilesValid() const noexcept {
        return (!watchedProfile_.stamp || profileError_.empty()) && !hasAccessoryError() && !accessoryAxisConflict;
    }

    bool enabled{};
    bool accessoryAxisConflict{};
    bool nativeShifter{};
    bool nativeShifterVerified{};
    bool nativeSequential{};
    std::optional<chrome_axis::ClutchMode> clutchMode;
    std::optional<chrome_axis::Profile> profile;
    std::optional<chrome_axis::ControlReader> reader;
    std::optional<chrome_axis::AccessoryProfile> accessoryProfile;
    std::optional<chrome_axis::AccessoryReader> accessories;
    chrome_axis::FeedbackSettings feedbackSettings;
    std::optional<DiagnosticFeedbackState> feedbackIssue;

private:
    void clearWatches() noexcept {
        watchedProfile_ = {};
        watchedAccessories_ = {};
        watchedFeedback_ = {};
    }

    std::filesystem::path directory_;
    chrome_axis::UsbProfileSelection selection_;
    WatchedProfile watchedProfile_, watchedAccessories_, watchedFeedback_;
    std::optional<chrome_axis::AccessoryProfile> savedAccessories_;
    std::string profileError_, accessoryError_;
    std::string lastProfileWarning_, lastAccessoryWarning_, lastFeedbackWarning_, lastAxisConflictWarning_;
    bool forceRefreshPending_{};
    bool reportedUsbState_{};
    bool reportedAccessoriesOnly_{};
    bool reportedRestorePending_{};
};

void reportChangedWarning(std::string& previous, std::string_view warning) {
    if (warning == previous)
        return;
    previous = warning;
    if (!previous.empty())
        warn(previous.c_str());
}

DiagnosticFeedbackState feedbackSettingsIssue(DWORD error) noexcept {
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
        return DiagnosticFeedbackState::settingsLocked;
    return error ? DiagnosticFeedbackState::settingsUnavailable : DiagnosticFeedbackState::settingsInvalid;
}

const char* feedbackSettingsWarning(DiagnosticFeedbackState issue) noexcept {
    switch (issue) {
    case DiagnosticFeedbackState::settingsLocked: return "Force feedback settings are temporarily locked.";
    case DiagnosticFeedbackState::settingsInvalid: return "Invalid force feedback settings.";
    default: return "Force feedback settings are unreadable.";
    }
}

UsbConfigurationChanges UsbConfiguration::refresh(const std::filesystem::path& root, Clock::time_point now, bool force,
                                                  chrome_axis::DiagnosticSnapshot& diagnostic) {
    UsbConfigurationChanges changes;
    const bool restorePending = chrome_axis::profileRestorePending(root);
    if (restorePending && !reportedRestorePending_)
        warn("USB paused while a profile operation needs recovery. Open Chrome Axis to resume it.");
    reportedRestorePending_ = restorePending;
    const bool requested = !restorePending && chrome_axis::usbEnabled(root);
    auto [selected, disabledFeedback, feedbackReadError] = selection_.refresh(root, requested, directory_,
                                                                               feedbackSettings.enabled, now);
    if (!selected) {
        forceRefreshPending_ = forceRefreshPending_ || force;
        if (disabledFeedback || feedbackReadError) {
            feedbackSettings = disabledFeedback.value_or(chrome_axis::FeedbackSettings{});
            if (feedbackReadError) {
                feedbackIssue = feedbackSettingsIssue(*feedbackReadError);
                reportChangedWarning(lastFeedbackWarning_, feedbackSettingsWarning(*feedbackIssue));
            }
            watchedFeedback_ = {};
            forceRefreshPending_ = true;
            changes.feedback = true;
        }
        return changes;
    }
    force = force || forceRefreshPending_;
    forceRefreshPending_ = false;
    changes.preset = selected->directory != directory_;
    if (changes.preset) {
        directory_ = std::move(selected->directory);
        diagnostic.profile.fill(0);
        if (!selected->name.empty()) {
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, selected->name.data(),
                static_cast<int>(selected->name.size()), diagnostic.profile.data(),
                static_cast<int>(diagnostic.profile.size() - 1), nullptr, nullptr);
        }
        clearWatches();
        if (hasProfileDirectory() && sdk && sdk->logger) {
            const auto path = directory_.u8string();
            sdk->logger->InfoF(plugin, "Active USB profile directory: %s", reinterpret_cast<const char*>(path.c_str()));
        }
    }

    force = force || changes.preset || requested != enabled;
    if (!reportedUsbState_ || requested != enabled) {
        info(requested ? "USB requested (Profil/usb.enabled found)."
                       : "USB not requested (Profil/usb.enabled absent or unreadable).");
        reportedUsbState_ = true;
    }
    if (requested && hasProfileDirectory()) {
        const auto profilePath = directory_ / chrome_axis::profileFilename;
        if (watchedProfile_.changed(profilePath, now, force)) {
            std::string error;
            DWORD readError{};
            auto updated = chrome_axis::loadProfile(profilePath, error, &readError);
            if (!watchedProfile_.deferLockedRead(readError, !changes.preset && profile.has_value(), now)) {
                profileError_ = std::move(error);
                watchedProfile_.readFinished(updated.has_value(), now, readError);
                changes.calibration = !sameProfile(profile, updated);
                if (changes.calibration) {
                    profile = std::move(updated);
                    reader.reset();
                    if (profile) {
                        reader.emplace(*profile);
                        info("USB wheel profile loaded. Release the controls to arm.");
                    }
                }
            }
        }
        const auto accessoryPath = directory_ / chrome_axis::accessoryFilename;
        bool readAccessories{};
        if (watchedAccessories_.changed(accessoryPath, now, force)) {
            std::string error;
            DWORD readError{};
            auto updated = chrome_axis::loadAccessoryProfile(accessoryPath, error, &readError);
            if (!watchedAccessories_.deferLockedRead(readError, !changes.preset && savedAccessories_.has_value(), now)) {
                if (readError == ERROR_FILE_NOT_FOUND || readError == ERROR_PATH_NOT_FOUND)
                    error.clear();
                accessoryError_ = std::move(error);
                savedAccessories_ = std::move(updated);
                watchedAccessories_.readFinished(savedAccessories_.has_value(), now, readError);
                readAccessories = true;
            }
        }
        if (readAccessories || changes.calibration) {
            auto updated = savedAccessories_;
            if (updated)
                chrome_axis::removeDisabledDrivingAccessories(*updated);
            // Preserve old settings on disk. Only conflicting analog accessory outputs are suppressed.
            accessoryAxisConflict = updated && chrome_axis::suppressConflictingAnalogAccessories(
                *updated, profile ? &*profile : nullptr);
            changes.accessories = !chrome_axis::sameAccessoryInputProfile(accessoryProfile, updated);
            accessoryProfile = std::move(updated);
            nativeShifter = accessoryProfile && std::ranges::any_of(*accessoryProfile, &chrome_axis::AccessoryBinding::nativeShifter);
            nativeShifterVerified = nativeShifter && chrome_axis::nativeShifterVerified(*accessoryProfile);
            nativeSequential = accessoryProfile &&
                std::ranges::any_of(*accessoryProfile, &chrome_axis::AccessoryBinding::nativeSequential);
            clutchMode.reset();
            if (accessoryProfile) {
                const auto clutch = std::ranges::find_if(*accessoryProfile,
                    [](const auto& binding) { return binding.clutchMode.has_value(); });
                if (clutch != accessoryProfile->end())
                    clutchMode = clutch->clutchMode;
            }
            if (changes.accessories) {
                accessories.reset();
                if (accessoryProfile && !accessoryProfile->empty()) {
                    accessories.emplace(*accessoryProfile);
                    info("USB accessory bindings loaded. Release the controls to arm.");
                }
            }
        }
        const auto feedbackPath = directory_ / chrome_axis::feedbackFilename;
        if (watchedFeedback_.changed(feedbackPath, now, force)) {
            std::string error;
            DWORD readError{};
            const auto updated = chrome_axis::loadFeedbackSettings(feedbackPath, error, &readError);
            watchedFeedback_.readFinished(updated.has_value(), now, readError);
            const auto next = updated.value_or(chrome_axis::FeedbackSettings{});
            const auto issue = updated ? std::nullopt : std::optional{feedbackSettingsIssue(readError)};
            changes.feedback = feedbackSettings != next || feedbackIssue != issue;
            feedbackSettings = next;
            feedbackIssue = issue;
            reportChangedWarning(lastFeedbackWarning_, feedbackIssue ? feedbackSettingsWarning(*feedbackIssue) : "");
        }
    } else {
        changes.calibration = profile.has_value();
        changes.accessories = accessoryProfile.has_value();
        changes.feedback = feedbackSettings.enabled || feedbackIssue.has_value();
        reader.reset();
        accessories.reset();
        profile.reset();
        accessoryProfile.reset();
        savedAccessories_.reset();
        accessoryAxisConflict = false;
        nativeShifter = false;
        nativeShifterVerified = false;
        nativeSequential = false;
        clutchMode.reset();
        clearWatches();
        feedbackSettings = {};
        feedbackIssue.reset();
        lastFeedbackWarning_.clear();
        profileError_ = requested ? "The selected USB profile is unavailable." : "";
        accessoryError_.clear();
    }

    const bool accessoriesOnly = requested && accessories && !reader && !watchedProfile_.stamp;
    if (accessoriesOnly && !reportedAccessoriesOnly_)
        info("USB accessories only. No analog calibration profile was found.");
    reportedAccessoriesOnly_ = accessoriesOnly;

    const auto analogWarning = !watchedProfile_.stamp && accessories
        ? std::string_view{} : std::string_view(profileError_);
    reportChangedWarning(lastProfileWarning_, analogWarning);
    reportChangedWarning(lastAccessoryWarning_, accessoryError_);
    reportChangedWarning(lastAxisConflictWarning_, accessoryAxisConflict
        ? "A clutch or handbrake axis conflicts with another command. Conflicting analog accessory commands were disabled."
        : "");
    enabled = requested;
    return changes;
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
    case AxisStatus::awaitingFirstReport: return DiagnosticAxisState::awaitingFirstReport;
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
    if (readiness.awaitingFirstReport)
        return DiagnosticAccessoryState::awaitingFirstReport;
    return ready ? DiagnosticAccessoryState::ready : DiagnosticAccessoryState::waiting;
}

DiagnosticCommandState diagnosticCommandState(chrome_axis::AccessoryStatus status) noexcept {
    using chrome_axis::AccessoryStatus;
    switch (status) {
    case AccessoryStatus::waitingForNeutral: return DiagnosticCommandState::waitingForNeutral;
    case AccessoryStatus::deviceUnavailable: return DiagnosticCommandState::deviceUnavailable;
    case AccessoryStatus::inputUnavailable: return DiagnosticCommandState::inputUnavailable;
    case AccessoryStatus::armed: return DiagnosticCommandState::ready;
    case AccessoryStatus::temporarilyUnavailable: return DiagnosticCommandState::temporarilyUnavailable;
    case AccessoryStatus::awaitingFirstReport: return DiagnosticCommandState::awaitingFirstReport;
    }
    return DiagnosticCommandState::inactive;
}

void describeClutchBlocker(chrome_axis::DiagnosticTransmission& transmission, chrome_axis::DiagnosticClutch& clutch,
                           std::optional<float> pressure) noexcept {
    using chrome_axis::DiagnosticClutchBlocker;
    using chrome_axis::DiagnosticShiftState;
    if (transmission.shift != DiagnosticShiftState::clutchRequired)
        return;

    const bool qualified = chrome_axis::ClutchShiftAssist::pedalQualified(pressure);
    if (!transmission.sequential && (transmission.selectedGear == -1 || qualified)) {
        // This selector position will be considered again on the next game update.
        transmission.shift = DiagnosticShiftState::pending;
    } else if (qualified) {
        // A momentary paddle request that already failed will not be repeated.
        clutch.blocker = DiagnosticClutchBlocker::retryPaddle;
    } else if (pressure) {
        clutch.blocker = DiagnosticClutchBlocker::belowThreshold;
    } else if (clutch.signal == DiagnosticCommandState::waitingForNeutral) {
        clutch.blocker = DiagnosticClutchBlocker::waitingForNeutral;
    } else if (clutch.signal == DiagnosticCommandState::temporarilyUnavailable ||
               clutch.signal == DiagnosticCommandState::awaitingFirstReport) {
        clutch.blocker = DiagnosticClutchBlocker::signalPending;
    } else {
        clutch.blocker = DiagnosticClutchBlocker::signalUnavailable;
    }
}

struct AccessoryOutputHistory {
    std::uint64_t sentAt{};
    Clock::time_point issueUntil{};
    DiagnosticCommandOutput issue{};
};

DiagnosticFeedbackState diagnosticFeedbackState(chrome_axis::WheelForceState state, bool steeringReady,
                                                 bool motionAvailable) noexcept {
    if (!steeringReady)
        return DiagnosticFeedbackState::waiting;
    if (!motionAvailable)
        return DiagnosticFeedbackState::waitingForMotion;
    switch (state) {
    case chrome_axis::WheelForceState::waiting: return DiagnosticFeedbackState::waitingForDriver;
    case chrome_axis::WheelForceState::active: return DiagnosticFeedbackState::active;
    case chrome_axis::WheelForceState::unavailable: return DiagnosticFeedbackState::unavailable;
    case chrome_axis::WheelForceState::neutral: return DiagnosticFeedbackState::neutral;
    }
    return DiagnosticFeedbackState::waiting;
}

std::uint64_t elapsedMicros(Clock::duration duration) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

template<std::size_t Size>
void copyDiagnosticToken(std::array<char, Size>& destination, std::string_view value) noexcept {
    std::ranges::copy(value.substr(0, Size - 1), destination.begin());
}

struct UsbReadDiagnostics {
    chrome_axis::DiagnosticPublisher& publisher;
    const UsbConfiguration& usb;

    void describeDevice(auto& entry, std::string_view id, std::string_view operation) const noexcept {
        copyDiagnosticToken(entry.deviceId, id);
        copyDiagnosticToken(entry.operation, operation);
        if (usb.profile) {
            const auto& profile = *usb.profile;
            const std::array axes{&profile.steering, &profile.throttle, &profile.brake};
            for (std::size_t i = 0; i < axes.size(); ++i) {
                if (axes[i]->deviceId == id)
                    entry.deviceAxisMask |= static_cast<std::uint8_t>(1u << i);
            }
        }
        if (usb.accessoryProfile) {
            entry.deviceAccessoryCount = static_cast<std::uint8_t>(std::ranges::count_if(*usb.accessoryProfile,
                [&](const auto& binding) { return binding.deviceId == id; }));
        }
    }

    void record(const chrome_axis::DirectInputReadEvent& read, std::uint64_t tick, DiagnosticMode mode = DiagnosticMode::usb,
                chrome_axis::DiagnosticUsbLane lane = chrome_axis::DiagnosticUsbLane::reader) noexcept {
        using ReadKind = chrome_axis::DirectInputReadEventKind;
        auto kind = DiagnosticUsbEventKind::deviceSlow;
        switch (read.kind) {
        case ReadKind::slow: break;
        case ReadKind::pending: kind = DiagnosticUsbEventKind::devicePending; break;
        case ReadKind::unavailable: kind = DiagnosticUsbEventKind::deviceUnavailable; break;
        case ReadKind::recovered: kind = DiagnosticUsbEventKind::deviceRecovered; break;
        case ReadKind::sampleExpired: kind = DiagnosticUsbEventKind::pollExpired; break;
        case ReadKind::discreteOverflow: kind = DiagnosticUsbEventKind::discreteOverflow; break;
        case ReadKind::layoutPending: kind = DiagnosticUsbEventKind::layoutPending; break;
        case ReadKind::layoutAcquireDenied: kind = DiagnosticUsbEventKind::layoutAcquireDenied; break;
        case ReadKind::layoutMissing: kind = DiagnosticUsbEventKind::layoutMissing; break;
        case ReadKind::layoutRecovered: kind = DiagnosticUsbEventKind::layoutRecovered; break;
        case ReadKind::workerStartFailed: kind = DiagnosticUsbEventKind::workerStartFailed; break;
        case ReadKind::workerStarted: kind = DiagnosticUsbEventKind::workerStarted; break;
        }
        DiagnosticUsbEvent event{.sampledAt = tick, .durationMicros = read.durationMicros,
                                 .result = static_cast<std::uint32_t>(read.result), .kind = kind,
                                 .mode = mode, .lane = lane,
                                 .hasCallLane = !read.operation.empty() && read.kind != ReadKind::discreteOverflow,
                                 .callStartedAt = read.call.start.tick, .callStartMode = read.call.start.mode};
        describeDevice(event, read.deviceId, read.operation);
        publisher.recordUsbEvent(event);
    }
};

// Called with stateMutex held, so the capture starts and stops atomically
// with respect to writes in the game callback.
void updatePerformanceCapture(chrome_axis::PerformanceReport& report, std::uint64_t& leaseUntil,
                              const std::optional<chrome_axis::PerformanceRequest>& request,
                              Clock::time_point now, std::uint64_t tick) noexcept {
    using chrome_axis::PerformanceState;
    if (request) {
        leaseUntil = request->leaseUntil;
        if (request->id && request->id != report.requestId) {
            if (request->enabled && tick < leaseUntil) {
                report = {};
                report.state = PerformanceState::capturing;
                report.startedAt = tick;
                inputAgeCapture = {now, {}};
            }
            report.requestId = request->id;
        }
    }
    if (report.state == PerformanceState::capturing) {
        report.inputAge = inputAgeCapture.timing;
        if (tick >= leaseUntil) {
            report.state = PerformanceState::complete;
            report.endedAt = tick;
            inputAgeCapture = {};
        }
    }
}

chrome_axis::DiagnosticTransmission diagnosticTransmission(const manual_backend::Snapshot& state,
                                                           bool driving, Clock::time_point now) noexcept {
    using chrome_axis::DiagnosticShiftState;
    const auto hotkey = chrome_axis::transmissionHotkeyBinding();
    const bool currentShortcut = transmissionShortcut.key == hotkey;
    const bool shortcutSource = inputFrame.nativeShifterVerified || inputFrame.nativeSequential || inputFrame.simulated;
    chrome_axis::DiagnosticTransmission result{
        .configured = inputFrame.nativeShifter || inputFrame.nativeSequential || inputFrame.simulated ||
            transmissionInput.hasSource(),
        .manual = state.manualActive,
        .engagedGear = state.gearCount > 0 ? state.engagedGear : -2,
        .sequential = inputFrame.nativeSequential,
        .shortcutKey = hotkey,
        .shortcutPressedAt = currentShortcut ? transmissionShortcut.pressedAt : 0,
        .shortcutPressCount = transmissionShortcut.pressCount,
        .shortcutSource = currentShortcut ? transmissionShortcut.source : chrome_axis::DiagnosticShortcutSource::none,
        .hasRequestedMode = transmissionInput.hasSource(),
        .requestedManual = transmissionInput.manualSelected(),
        .shortcutAvailable = transmissionAvailable && driving && shortcutSource &&
            (chrome_axis::transmissionHotkeyAvailable() || accessoryKeys.allowsTransmissionKeyPolling(hotkey)),
        .gearboxSampledAt = state.sampledAt};
    if (!result.configured)
        return result;
    if (!transmissionAvailable) {
        result.shift = DiagnosticShiftState::unavailable;
        return result;
    }

    const auto leverGear = sourceInputFresh(inputFrame.gearSampledAt, now) ? inputFrame.nativeGear : std::nullopt;
    const bool paddlesReady = inputFrame.sequentialArmed && sourceInputFresh(inputFrame.sequentialSampledAt, now);
    result.selectedGear = inputFrame.nativeShifter ? leverGear.value_or(-2) : state.selectedGear.value_or(-2);
    result.requestedGear = state.requestedGear;
    if (inputFrame.nativeShifter && !inputFrame.nativeShifterVerified) {
        result.shift = DiagnosticShiftState::recalibrationNeeded;
        return result;
    }
    if (inputFrame.selectorAmbiguous) {
        result.shift = DiagnosticShiftState::selectorAmbiguous;
        return result;
    }
    if ((inputFrame.nativeShifter && !leverGear) || (inputFrame.nativeSequential && !paddlesReady)) {
        result.shift = DiagnosticShiftState::selectorUnavailable;
        return result;
    }
    if (clutchShiftBlocked) {
        result.shift = DiagnosticShiftState::clutchRequired;
        return result;
    }
    if (state.requestPending || (state.manualRequested && paddleRequests.pending())) {
        result.shift = DiagnosticShiftState::pending;
        return result;
    }
    if (paddleRequests.expired()) {
        result.shift = DiagnosticShiftState::expired;
        return result;
    }
    constexpr std::array outcomes{
        std::pair{"manual_ready", DiagnosticShiftState::ready},
        std::pair{"outside_supported_range", DiagnosticShiftState::outsideRange},
        std::pair{"direction_change_requires_stop", DiagnosticShiftState::stopToChangeDirection},
        std::pair{"already_engaged", DiagnosticShiftState::alreadyEngaged},
        std::pair{"native_rejected", DiagnosticShiftState::rejected},
        std::pair{"native_accepted", DiagnosticShiftState::accepted},
        std::pair{"neutral_transition", DiagnosticShiftState::neutralTransition},
        std::pair{"request_expired", DiagnosticShiftState::expired},
        std::pair{"request_cancelled", DiagnosticShiftState::expired},
        std::pair{"engagement_unconfirmed", DiagnosticShiftState::expired},
    };
    const auto outcome = std::ranges::find_if(outcomes, [&](const auto& entry) {
        return std::string_view(state.outcome) == entry.first;
    });
    if (outcome != outcomes.end())
        result.shift = outcome->second;
    if (result.shift != DiagnosticShiftState::expired &&
        ((state.manualRequested && !state.manualActive) ||
         (transmissionPedals.tracking() && pedalGear(state) == chrome_axis::PedalGear::unavailable)))
        result.shift = DiagnosticShiftState::unavailable;
    return result;
}

DiagnosticCommandOutput nativeGearOutput(const chrome_axis::DiagnosticTransmission& transmission, int gear) noexcept {
    using chrome_axis::DiagnosticShiftState;
    if (!transmission.manual)
        return DiagnosticCommandOutput::nativePaused;
    if (transmission.selectedGear != gear)
        return DiagnosticCommandOutput::nativeReady;
    if (transmission.engagedGear == gear)
        return DiagnosticCommandOutput::nativeEngaged;
    if (transmission.requestedGear == gear) {
        switch (transmission.shift) {
        case DiagnosticShiftState::outsideRange:
        case DiagnosticShiftState::stopToChangeDirection:
        case DiagnosticShiftState::rejected:
        case DiagnosticShiftState::expired:
        case DiagnosticShiftState::clutchRequired:
            return DiagnosticCommandOutput::nativeRefused;
        default:
            break;
        }
    }
    return DiagnosticCommandOutput::nativePending;
}

void pollInput(std::stop_token stop, const std::filesystem::path& configurationDirectory, DriverWorkers& workers) {
    const auto executable = chrome_axis::modulePath(nullptr);
    const auto gameRoot = executable.parent_path().parent_path().parent_path();
    std::optional<chrome_axis::DiagnosticPublisher> diagnostics;
    try {
        if (!gameRoot.empty())
            diagnostics.emplace(gameRoot);
    } catch (const std::exception&) {
        warn("Could not initialize the Chrome Axis diagnostics channel.");
    }
    UsbConfiguration usb;
    std::optional<UsbReadDiagnostics> usbReadDiagnostics;
    if (diagnostics)
        usbReadDiagnostics.emplace(UsbReadDiagnostics{*diagnostics, usb});
    std::optional<std::pair<DiagnosticMode, bool>> lastUsbContext;
    std::optional<DiagnosticUsbEvent> lastUsbReadiness;
    std::uint64_t usbWaitStartedAt{};
    chrome_axis::DiagnosticSnapshot diagnostic;
    chrome_axis::PerformanceReport performance;
    chrome_axis::FeedbackMeasurement feedbackMeasurement;
    std::uint64_t performanceLeaseUntil{};
    Clock::time_point nextDiagnostic{};
    Clock::time_point lastUsbPollFinished{};
    Clock::time_point accessoryBlockedUntil{};
    Clock::time_point modifierBlockedUntil{};
    Clock::time_point nextTimeoutReport{};
    Clock::time_point nextPendingReport{};
    std::uint64_t diagnosticSession{};
    std::array<AccessoryOutputHistory, 256> accessoryHistory{};
    std::bitset<256> previousHeldKeys;
    AccessoryPresses accessoryPresses;
    std::array<chrome_axis::DiagnosticUsbOperation, chrome_axis::diagnosticUsbOperationCapacity> usbOperations;
    const auto publishDiagnostics = [&](HWND foreground, const DrivingContext& polled) {
        const auto now = Clock::now();
        nextDiagnostic = now + (performance.state == chrome_axis::PerformanceState::capturing
            ? captureInterval : diagnosticInterval);
        if (!diagnostics || !active.load(std::memory_order_acquire))
            return;
        const bool gameForeground = foregroundGameWindow() != nullptr;
        auto snapshot = diagnostic;
        std::size_t activeOperationCount = 0;
        // Reserve the force feedback lane even if readers fill the remaining slots.
        const auto feedbackOperation = workers.force.activeOperation();
        if (feedbackOperation.call.overdue(GetTickCount64(), 250)) {
            auto& entry = usbOperations[activeOperationCount++];
            entry = {.startedAt = feedbackOperation.call.startedAt, .lane = chrome_axis::DiagnosticUsbLane::feedback,
                     .startMode = feedbackOperation.call.startMode};
            usbReadDiagnostics->describeDevice(entry, feedbackOperation.deviceId.data(), feedbackOperation.call.operation);
        }
        if (workers.input) {
            workers.input->visitActiveCalls([&](std::string_view id, const auto& call, bool layoutProbe) {
                if (activeOperationCount == usbOperations.size())
                    return;
                auto& entry = usbOperations[activeOperationCount++];
                entry = {.startedAt = call.startedAt, .lane = layoutProbe ? chrome_axis::DiagnosticUsbLane::probe
                                                                         : chrome_axis::DiagnosticUsbLane::reader,
                         .startMode = call.startMode};
                usbReadDiagnostics->describeDevice(entry, id, call.operation);
            });
        }
        const auto performanceRequest = diagnostics->performanceRequest();
        std::uint64_t drivingSession{};
        std::optional<chrome_axis::DrivingCaptureSample> captureSample;
        bool menuPaused = false;
        bool validDrivingSample = false;
        {
            std::scoped_lock lock(stateMutex);
            // A vehicle transition can invalidate the worker's sample before publishing.
            if (gameForeground && foreground && context.generation != polled.generation)
                return;
            const auto tick = GetTickCount64();
            if (performanceRequest || performance.state == chrome_axis::PerformanceState::capturing)
                updatePerformanceCapture(performance, performanceLeaseUntil, performanceRequest, now, tick);
            const bool capturing = performance.state == chrome_axis::PerformanceState::capturing;
            if (drivingCaptureActive.exchange(capturing, std::memory_order_relaxed) != capturing)
                captureVehicleLabel = {};
            if (!gameForeground) {
                snapshot.mode = DiagnosticMode::gameNotForeground;
            } else if (!foreground) {
                snapshot.mode = DiagnosticMode::waitingForDriver;
                menuPaused = context.menuPaused;
            } else {
                drivingSession = context.session;
                if (chrome_axis::isDrivingCaptureMode(snapshot.mode) && !vehicleInputFresh(context, now)) {
                    snapshot.mode = DiagnosticMode::waitingForVehicleInput;
                }
            }
            if (!drivingSession) {
                if (snapshot.feedback != DiagnosticFeedbackState::disabled && !usb.feedbackIssue)
                    snapshot.feedback = DiagnosticFeedbackState::inactive;
                snapshot.feedbackData = {};
            } else if (snapshot.mode == DiagnosticMode::waitingForVehicleInput &&
                       (snapshot.feedback == DiagnosticFeedbackState::active ||
                        snapshot.feedback == DiagnosticFeedbackState::neutral ||
                        snapshot.feedback == DiagnosticFeedbackState::waitingForDriver)) {
                snapshot.feedback = DiagnosticFeedbackState::waitingForMotion;
                snapshot.feedbackData = {};
            }
            if (context.session != diagnosticSession) {
                diagnosticSession = context.session;
                diagnostic.lastAccessoryKey = 0;
                diagnostic.accessorySentAt = 0;
                accessoryHistory = {};
                snapshot.lastAccessoryKey = 0;
                snapshot.accessorySentAt = 0;
                snapshot.feedbackData = {};
                diagnostic.feedbackData = {};
            }
            const auto gearbox = drivingSession && (inputFrame.nativeShifter || inputFrame.nativeSequential ||
                inputFrame.simulated || capturing) ? manual_backend::snapshot(context.vehicle) : manual_backend::Snapshot{};
            const bool drivingInputFresh = drivingSession && vehicleInputFresh(context, now) &&
                inputFrame.generation == context.generation && now - inputFrame.sampled <= inputLifetime;
            snapshot.transmission = diagnosticTransmission(gearbox, drivingInputFresh, now);
            const bool hasDelivery = drivingSession && vehicleDelivery.sampled != Clock::time_point{} &&
                now - vehicleDelivery.sampled <= inputLifetime;
            const bool liveUsb = drivingSession && hudFrame.generation == context.generation &&
                hudFrame.sampled != Clock::time_point{} && now - hudFrame.sampled <= inputLifetime;
            if (snapshot.mode == DiagnosticMode::usb && liveUsb) {
                if (hudFrame.status == HudStatus::usbReady)
                    snapshot.usbReadiness = chrome_axis::DiagnosticUsbReadiness::ready;
                else if (hudFrame.status == HudStatus::usbPartial)
                    snapshot.usbReadiness = chrome_axis::DiagnosticUsbReadiness::partial;
            }
            const bool axesOrHandbrakeDelivered = hasDelivery &&
                (vehicleDelivery.handbrake.has_value() || std::ranges::any_of(vehicleDelivery.axes,
                    [](const AxisDelivery& axis) { return axis.outcome != DiagnosticDelivery::unavailable; }));
            const bool nativeSelectorObserved = gearbox.sampledAt && tick >= gearbox.sampledAt &&
                tick - gearbox.sampledAt <= static_cast<std::uint64_t>(inputLifetime.count()) &&
                ((inputFrame.nativeGear.has_value() && sourceInputFresh(inputFrame.gearSampledAt, now)) ||
                 (inputFrame.sequentialArmed && sourceInputFresh(inputFrame.sequentialSampledAt, now)));
            const bool keySent = snapshot.accessorySentAt && tick >= snapshot.accessorySentAt &&
                tick - snapshot.accessorySentAt <= static_cast<std::uint64_t>(inputLifetime.count());
            const bool keyHeld = std::ranges::any_of(std::span(snapshot.accessoryCommands).first(snapshot.accessoryCount),
                [](const auto& command) {
                    return command.state == DiagnosticCommandState::ready &&
                        (command.output == DiagnosticCommandOutput::held || command.output == DiagnosticCommandOutput::pulsing);
                });
            validDrivingSample = drivingSession && chrome_axis::canRetainDrivingSample(snapshot.mode,
                liveUsb && hudFrame.status == HudStatus::usbReady,
                liveUsb && hudFrame.status == HudStatus::usbPartial &&
                    (axesOrHandbrakeDelivered || nativeSelectorObserved || keySent || keyHeld));
            const auto clutch = inputFrame.simulated ? (drivingSession ? probePosition : std::nullopt) :
                (drivingInputFresh && sourceInputFresh(inputFrame.clutchSampledAt, now) ? inputFrame.clutch : std::nullopt);
            const auto attenuation = hasDelivery ? vehicleDelivery.clutchAttenuation : std::nullopt;
            snapshot.clutch = {
                .input = clutch.value_or(0.0f),
                .applied = attenuation.value_or(0.0f),
                .configured = inputFrame.clutchMode.has_value() || (inputFrame.simulated && probePosition.has_value()),
                .inputAvailable = clutch.has_value(),
                .assistanceEnabled = inputFrame.clutchMode == chrome_axis::ClutchMode::assist,
                .appliedAvailable = attenuation.has_value(),
                .probeActive = inputFrame.clutchMode == chrome_axis::ClutchMode::experiment ||
                    (inputFrame.simulated && probePosition.has_value()),
            };
            if (drivingInputFresh && inputFrame.clutchObservation) {
                const auto& observed = *inputFrame.clutchObservation;
                if (sourceInputFresh(observed.sampledAt, now)) {
                    snapshot.clutch.signal = diagnosticCommandState(observed.status);
                    if (observed.travel && observed.neutral) {
                        snapshot.clutch.rawTravel = static_cast<float>(*observed.travel);
                        snapshot.clutch.rest = *observed.neutral ? chrome_axis::DiagnosticClutchRest::inRange
                                                                : chrome_axis::DiagnosticClutchRest::outOfRange;
                    }
                } else {
                    snapshot.clutch.signal = DiagnosticCommandState::sampleExpired;
                }
            }
            if (drivingInputFresh)
                describeClutchBlocker(snapshot.transmission, snapshot.clutch, clutch);
            if (capturing && drivingSession && vehicleInputFresh(context, now)) {
                const bool finalPedals = hasDelivery && vehicleDelivery.pedalTick;
                const bool steeringApplied = hasDelivery && vehicleDelivery.axes[0].outcome == DiagnosticDelivery::applied;
                const auto handbrake = hasDelivery ? vehicleDelivery.handbrake : std::nullopt;
                const bool feedbackApplied = snapshot.feedback == DiagnosticFeedbackState::active &&
                    snapshot.feedbackData.submittedAt && tick >= snapshot.feedbackData.submittedAt &&
                    tick - snapshot.feedbackData.submittedAt <=
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(inputLifetime).count());
                captureSample = chrome_axis::DrivingCaptureSample{
                    .sampledAt = tick,
                    .mode = snapshot.mode,
                    .vehicle = captureVehicleLabel,
                    .speedMps = gearbox.speedMetersPerSecond.value_or(0.0f),
                    .rpm = gearbox.engineRpm.value_or(0.0f),
                    .throttle = finalPedals ? vehicleDelivery.finalPedals[0] : 0.0f,
                    .brake = finalPedals ? vehicleDelivery.finalPedals[1] : 0.0f,
                    .speedAvailable = gearbox.speedMetersPerSecond.has_value(),
                    .rpmAvailable = gearbox.engineRpm.has_value(),
                    .throttleAvailable = finalPedals,
                    .brakeAvailable = finalPedals,
                    .transmission = snapshot.transmission,
                    .clutch = snapshot.clutch,
                    .shiftDelayMicros = gearbox.engagementDelayMs.value_or(0) * 1000,
                    .shiftDelayAvailable = gearbox.engagementDelayMs.has_value(),
                    .steering = steeringApplied ? appliedAxes[0].value : 0.0f,
                    .handbrake = handbrake ? handbrake->finalValue : 0.0f,
                    .feedbackForce = feedbackApplied ? snapshot.feedbackData.submitted : 0.0f,
                    .steeringAvailable = steeringApplied,
                    .handbrakeAvailable = handbrake.has_value(),
                    .feedbackForceAvailable = feedbackApplied,
                    .feedback = snapshot.feedback,
                };
            } else if (capturing) {
                captureSample = chrome_axis::DrivingCaptureSample{.sampledAt = tick, .mode = snapshot.mode,
                    .transmission = snapshot.transmission, .feedback = snapshot.feedback};
            }
            const bool hasPedalDelivery = hasDelivery && snapshot.mode == DiagnosticMode::usb &&
                snapshot.profileLoaded && vehicleDelivery.pedalTick;
            if (hasPedalDelivery)
                snapshot.pedalSampledAt = vehicleDelivery.pedalTick;
            if (hasDelivery && snapshot.mode == DiagnosticMode::usb && inputFrame.handbrake && vehicleDelivery.handbrake) {
                for (auto& command : std::span(snapshot.accessoryCommands).first(snapshot.accessoryCount)) {
                    if (command.handbrakeCeilingPercent && command.state == DiagnosticCommandState::ready) {
                        command.handbrakeRequested = vehicleDelivery.handbrake->requested;
                        command.handbrakeFinal = vehicleDelivery.handbrake->finalValue;
                        command.handbrakeAvailable = 1;
                    }
                }
            }
            for (std::size_t i = 0; i < appliedAxes.size(); ++i) {
                auto& axis = snapshot.axes[i];
                axis.value = appliedAxes[i].value;
                axis.appliedAt = appliedAxes[i].tick;
                if (!drivingSession) {
                    axis.state = DiagnosticAxisState::inactive;
                    axis.inputAvailable = 0;
                }
                if (!liveUsb || snapshot.mode != DiagnosticMode::usb)
                    axis.pedalRest = {};
                if (hasPedalDelivery && i > 0) {
                    axis.finalValue = vehicleDelivery.finalPedals[i - 1];
                    axis.finalAvailable = 1;
                    const auto& reading = vehicleDelivery.axes[i].reading;
                    axis.inputValue = reading.value_or(0.0f);
                    axis.inputAvailable = reading.has_value();
                }
                if (hasDelivery && ((hasPedalDelivery && i > 0) || axis.state == DiagnosticAxisState::armed ||
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
        if (snapshot.mode == DiagnosticMode::usb && snapshot.transmission.configured) {
            for (auto& row : std::span(snapshot.accessoryCommands).first(snapshot.accessoryCount)) {
                const auto position = chrome_axis::nativeShifterPositionBit(row.role);
                if (!row.key && position) {
                    row.output = nativeGearOutput(snapshot.transmission, std::countr_zero(position));
                } else if (snapshot.transmission.sequential && !row.key &&
                           (row.role == chrome_axis::ShifterRole::Upshift || row.role == chrome_axis::ShifterRole::Downshift)) {
                    using chrome_axis::DiagnosticShiftState;
                    row.output = !snapshot.transmission.manual ? DiagnosticCommandOutput::nativePaused
                        : row.active && snapshot.transmission.shift == DiagnosticShiftState::pending
                            ? DiagnosticCommandOutput::nativePending : DiagnosticCommandOutput::nativeReady;
                }
            }
        }
        if (!drivingSession || snapshot.mode != DiagnosticMode::usb) {
            for (std::size_t i = 0; i < snapshot.accessoryCount; ++i) {
                auto& row = snapshot.accessoryCommands[i];
                row.state = snapshot.mode == DiagnosticMode::waitingForVehicleInput
                    ? DiagnosticCommandState::waitingForVehicle : DiagnosticCommandState::inactive;
                row.output = DiagnosticCommandOutput::none;
                row.activeAvailable = 0;
                row.travelAvailable = 0;
                row.lastSentAt = 0;
            }
        }
        if (!drivingSession) {
            snapshot.accessories = DiagnosticAccessoryState::inactive;
            snapshot.accessoryArmed = 0;
        }
        if (usb.enabled && snapshot.mode != DiagnosticMode::simulation) {
            const auto usbContext = std::pair{snapshot.mode, menuPaused};
            if (lastUsbContext != usbContext) {
                diagnostics->recordUsbEvent({
                    .sampledAt = GetTickCount64(),
                    .kind = menuPaused ? DiagnosticUsbEventKind::menuPaused : DiagnosticUsbEventKind::contextChanged,
                    .mode = snapshot.mode
                });
                lastUsbContext = usbContext;
            }
        } else {
            lastUsbContext.reset();
        }
        if (captureSample) {
            for (std::size_t i = 0; i < snapshot.axes.size(); ++i)
                captureSample->axes[i] = snapshot.axes[i].state;
            diagnostics->recordDrivingSample(performance.requestId, *captureSample);
        }
        diagnostics->publish(snapshot, drivingSession, performance, validDrivingSample,
                             std::span(usbOperations).first(activeOperationCount));
    };
    chrome_axis::WheelFeedback wheelFeedback;
    auto& feedbackOutput = workers.force;
    auto feedbackState = chrome_axis::WheelForceState::waiting;
    Clock::time_point lastFeedbackUpdate{};
    std::uint64_t feedbackConnection{};
    std::uint64_t firstForceRequestedAt{};
    bool firstEffectAccepted{};
    const auto resetFeedback = [&] {
        feedbackOutput.cancel();
        wheelFeedback.reset();
        feedbackState = chrome_axis::WheelForceState::waiting;
        lastFeedbackUpdate = {};
        feedbackConnection = 0;
        firstForceRequestedAt = 0;
        firstEffectAccepted = false;
        diagnostic.feedbackData = {};
    };
    const auto collectFeedback = [&] {
        while (const auto operation = feedbackOutput.nextOperation()) {
            if (usbReadDiagnostics) {
                usbReadDiagnostics->record({chrome_axis::DirectInputReadEventKind::slow,
                    operation->deviceId.data(), operation->name.data(), operation->result,
                    operation->durationMicros, {.start = operation->start}},
                    operation->sampledAt, operation->mode, chrome_axis::DiagnosticUsbLane::feedback);
            }
        }
        while (const auto result = feedbackOutput.nextCompletion()) {
            const bool capture = result->captureId && result->captureId == performance.requestId &&
                performance.state == chrome_axis::PerformanceState::capturing;
            if (capture) {
                performance.feedback.add(result->durationMicros);
                performance.feedbackIntervalOverruns += result->missedCadence ||
                    result->durationMicros > static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(feedbackInterval).count());
            }
            if (result->generation != feedbackOutput.generation())
                continue;
            feedbackState = result->state;
            if (result->accepted) {
                diagnostic.feedbackData.submittedAt = result->submittedAt;
                diagnostic.feedbackData.submitted = *result->accepted;
                if (!firstEffectAccepted && firstForceRequestedAt && result->submittedAt >= firstForceRequestedAt) {
                    firstEffectAccepted = true;
                    if (usbReadDiagnostics && usb.profile) {
                        DiagnosticUsbEvent event{.sampledAt = result->submittedAt,
                            .durationMicros = (result->submittedAt - firstForceRequestedAt) * 1000,
                            .kind = DiagnosticUsbEventKind::feedbackReady, .mode = DiagnosticMode::usb,
                            .lane = chrome_axis::DiagnosticUsbLane::feedback, .hasCallLane = true};
                        usbReadDiagnostics->describeDevice(event, usb.profile->steering.deviceId, {});
                        usbReadDiagnostics->publisher.recordUsbEvent(event);
                    }
                }
                if (capture) {
                    ++performance.submittedEffects;
                    performance.peakSubmittedForce = std::max(performance.peakSubmittedForce,
                                                               std::abs(*result->accepted));
                }
            }
        }
        if (feedbackState == chrome_axis::WheelForceState::active &&
            GetTickCount64() - diagnostic.feedbackData.submittedAt >
                static_cast<std::uint64_t>(inputLifetime.count()))
            feedbackState = chrome_axis::WheelForceState::waiting;
    };
    chrome_axis::SimulationReceiver simulation(gameRoot);
    auto& input = workers.input;
    const auto collectInputEvents = [&] {
        if (!input)
            return;
        input->drainEvents([&](const std::string& id, const auto& event) {
            if (performance.state == chrome_axis::PerformanceState::capturing &&
                event.sampledAt >= performance.startedAt &&
                event.kind == chrome_axis::DirectInputReadEventKind::sampleExpired)
                ++performance.expiredReads;
            if (usbReadDiagnostics)
                usbReadDiagnostics->record({event.kind, id, event.operation.data(), event.result,
                    event.durationMicros, {.start = {event.callStartedAt, event.callStartMode}}},
                    event.sampledAt, event.mode.value_or(usbReaderObservationMode()),
                    event.probe ? chrome_axis::DiagnosticUsbLane::probe : chrome_axis::DiagnosticUsbLane::reader);
        });
    };
    struct StopWorkers {
        DriverWorkers& workers;

        ~StopWorkers() {
            feedbackRequested.store(false, std::memory_order_relaxed);
            workers.force.requestStop();
            if (workers.input)
                workers.input->requestStop();
            accessoryKeys.apply({});
        }
    } stopWorkers{workers};
    std::vector<chrome_axis::DrivingInputDevice> inputDevices;
    bool inputConfigured{};
    bool inputActive{};
    HWND window{};
    Clock::time_point nextConfigurationPoll{};
    Clock::time_point nextHotkeyPoll{};
    WatchedProfile watchedHotkey;
    const auto hotkeyPath = configurationDirectory / chrome_axis::transmissionHotkeyFilename;
    std::uint64_t lastGeneration{};
    bool wasDriving{};
    bool wasSimulated{};
    chrome_axis::TransmissionSource usedTransmissionSource{};
    bool reportedDriver{};
    std::optional<std::array<chrome_axis::AxisStatus, 3>> lastAxisStatuses;
    Clock::time_point nextAxisLog{};

    while (!stop.stop_requested()) {
        const auto start = Clock::now();
        if (start >= nextHotkeyPoll) {
            if (watchedHotkey.changed(hotkeyPath, start, nextHotkeyPoll == Clock::time_point{})) {
                const auto binding = chrome_axis::readTransmissionHotkey(configurationDirectory);
                std::error_code error;
                const bool missing = !binding && !watchedHotkey.stamp &&
                    !std::filesystem::exists(hotkeyPath, error) && !error;
                if (binding || missing)
                    chrome_axis::selectTransmissionHotkey(binding.value_or(chrome_axis::defaultTransmissionHotkey));
                watchedHotkey.retryAt = binding || missing ? Clock::time_point::max() : start + 1s;
            }
            nextHotkeyPoll = start + configurationPollInterval;
        }
        DrivingContext current;
        {
            std::scoped_lock lock(stateMutex);
            current = context;
        }
        const HWND gameForeground = foregroundGameWindow();
        const HWND foreground = current.vehicle && start - current.refreshed <= contextLifetime
            ? gameForeground : nullptr;
        if (current.session != diagnosticSession) {
            diagnosticSession = current.session;
            diagnostic.lastAccessoryKey = 0;
            diagnostic.accessorySentAt = 0;
            accessoryHistory = {};
            diagnostic.feedbackData = {};
        }
        const auto session = simulation.session();
        const bool simulated = session != chrome_axis::SimulationSession::inactive;
        const bool modeChanged = simulated != wasSimulated;
        UsbConfigurationChanges changes;
        if (!simulated && (modeChanged || (foreground && !wasDriving) || start >= nextConfigurationPoll)) {
            nextConfigurationPoll = start + configurationPollInterval;
            changes = usb.refresh(configurationDirectory, start, modeChanged || (foreground && !wasDriving), diagnostic);
            // A settings change can be observed before the next foreground driving sample.
            if (changes.feedback || changes.calibration || changes.preset)
                feedbackMeasurement.invalidate(performance);
        }
        const bool physical = !simulated && (usb.reader || usb.accessories);
        const auto diagnosticSource = simulated ? DiagnosticMode::simulation : !usb.enabled ? DiagnosticMode::usbDisabled
            : !physical ? DiagnosticMode::profileUnavailable : DiagnosticMode::usb;
        feedbackSource.store(diagnosticSource, std::memory_order_release);
        const auto readerMode = !gameForeground ? DiagnosticMode::gameNotForeground :
            !foreground ? DiagnosticMode::waitingForDriver : !vehicleInputFresh(current, start)
                ? DiagnosticMode::waitingForVehicleInput : diagnosticSource;
        usbReaderMode.store(readerMode, std::memory_order_release);
        const auto source = transmissionAvailable
            ? chrome_axis::transmissionSource(simulated, usb.nativeShifterVerified, usb.nativeSequential)
            : chrome_axis::TransmissionSource::none;
        if (source != usedTransmissionSource || session == chrome_axis::SimulationSession::restarted) {
            std::scoped_lock lock(stateMutex);
            if (context.generation == current.generation) {
                chrome_axis::setTransmissionHotkeyContext(false);
                suspendTransmission();
                invalidateFrames();
                if (session == chrome_axis::SimulationSession::restarted)
                    transmissionInput = {};
                transmissionInput.selectSource(source);
                usedTransmissionSource = source;
            }
        }
        if (!foreground) {
            wasSimulated = simulated;
            modifierBlockedUntil = {};
            lastUsbReadiness.reset();
            usbWaitStartedAt = 0;
            lastUsbPollFinished = {};
            feedbackRequested.store(false, std::memory_order_relaxed);
            previousHeldKeys.reset();
            accessoryPresses.clear();
            if (accessoryKeys.hasInjectedKeys()) {
                accessoryKeys.apply({});
            }
            if (wasDriving) {
                resetFeedback();
                if (input)
                    input->pause();
                inputConfigured = false;
                inputActive = false;
                usb.resetReaders();
                std::scoped_lock lock(stateMutex);
                if (context.generation == current.generation) {
                    invalidateFrames();
                }
            }
            collectFeedback();
            collectInputEvents();
            wasDriving = false;
            lastAxisStatuses.reset();
            if (diagnostics && (start >= nextDiagnostic || changes.feedback)) {
                diagnostic.feedback = !simulated && usb.feedbackIssue ? *usb.feedbackIssue
                    : usb.feedbackSettings.enabled ? DiagnosticFeedbackState::inactive : DiagnosticFeedbackState::disabled;
                publishDiagnostics(foreground, current);
            }
            std::this_thread::sleep_until(start + idlePollInterval);
            continue;
        }
        if (!reportedDriver) {
            info("Player driver context detected with the game in the foreground.");
            reportedDriver = true;
        }

        const bool windowChanged = foreground != window;
        const bool drivingContextChanged = current.generation != lastGeneration;
        const bool sourceChanged = modeChanged || session == chrome_axis::SimulationSession::restarted || changes.preset;
        if (!wasDriving || drivingContextChanged || sourceChanged || windowChanged ||
            changes.calibration || changes.accessories) {
            accessoryKeys.apply({});
            previousHeldKeys.reset();
            accessoryPresses.clear();
            if (input)
                input->discardAccessorySamples();
            resetFeedback();
            usb.resetReaders();
            lastUsbPollFinished = {};
            lastUsbReadiness.reset();
            usbWaitStartedAt = 0;
            std::scoped_lock lock(stateMutex);
            if (context.generation == current.generation) {
                invalidateFrames();
                if (sourceChanged || changes.calibration)
                    appliedAxes = {};
            }
            if (sourceChanged || changes.accessories) {
                diagnostic.lastAccessoryKey = 0;
                diagnostic.accessorySentAt = 0;
                accessoryBlockedUntil = {};
                modifierBlockedUntil = {};
                accessoryHistory = {};
            }
            if (sourceChanged || changes.calibration)
                nextTimeoutReport = {};
            nextPendingReport = {};
            lastGeneration = current.generation;
        }
        if (windowChanged) {
            if (input && usb.enabled && !simulated && diagnostics) {
                DiagnosticUsbEvent event{.sampledAt = GetTickCount64(), .kind = DiagnosticUsbEventKind::directInputReopened,
                                         .mode = DiagnosticMode::usb};
                copyDiagnosticToken(event.operation, "window");
                diagnostics->recordUsbEvent(event);
            }
            window = foreground;
            inputConfigured = false;
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
        std::array<chrome_axis::PedalRestReading, 2> pedalRestReadings{};
        auto sampledAt = start;
        bool sampleExpired = false;
        std::bitset<256> requestedKeys;
        std::bitset<256> replayReleases;
        chrome_axis::AccessoryKeyTimes keyTimes{};
        std::optional<float> handbrake;
        std::optional<int> nativeGear;
        chrome_axis::AccessoryTransmissionSample transmissionSample;
        chrome_axis::PaddleInputBatch paddleBatch;
        bool paddleOverflow{};
        std::array<chrome_axis::AccessoryObservation, chrome_axis::maxAccessoryBindings> accessoryObservations{};
        bool readAccessories = false;
        const bool measuring = performance.state == chrome_axis::PerformanceState::capturing;
        const bool wantFeedback = physical && usb.feedbackSettings.enabled && usb.profile &&
            !usb.profile->steering.deviceId.empty();
        feedbackRequested.store(wantFeedback, std::memory_order_relaxed);
        if (!physical) {
            lastUsbPollFinished = {};
            if (input && inputActive) {
                input->pause();
                inputConfigured = false;
                inputActive = false;
            }
        }
        if (changes.feedback) {
            resetFeedback();
            std::scoped_lock lock(stateMutex);
            if (context.generation == current.generation) {
                vehicleMotion = {};
            }
        }
        if (physical) {
            if (!input)
                input = std::make_unique<chrome_axis::DrivingInputs<>>(physicalPollInterval, inputLifetime,
                    &logDirectInput, &usbReaderObservationMode);
            if (!inputActive || windowChanged || sourceChanged || changes.calibration || changes.accessories) {
                inputConfigured = false;
                inputDevices.clear();
                const auto deviceFor = [&](const std::string& id) -> chrome_axis::DrivingInputDevice& {
                    const auto it = std::ranges::find(inputDevices, id, &chrome_axis::DrivingInputDevice::id);
                    if (it != inputDevices.end())
                        return *it;
                    return inputDevices.emplace_back(chrome_axis::DrivingInputDevice{.id = id});
                };
                if (usb.profile) {
                    for (const auto* axis : {&usb.profile->steering, &usb.profile->throttle, &usb.profile->brake})
                        if (!axis->deviceId.empty())
                            deviceFor(axis->deviceId).objects.axes.set(axis->axis);
                }
                if (usb.accessoryProfile) {
                    for (const auto& binding : *usb.accessoryProfile) {
                        auto& device = deviceFor(binding.deviceId);
                        auto& mask = device.objects;
                        switch (binding.kind) {
                        case chrome_axis::AccessoryKind::Axis: mask.axes.set(binding.index); break;
                        case chrome_axis::AccessoryKind::Button: mask.buttons.set(binding.index); break;
                        case chrome_axis::AccessoryKind::Pov: mask.povs.set(binding.index); break;
                        }
                        if (chrome_axis::isDiscreteAccessory(binding)) {
                            if (!device.discrete)
                                device.discrete = std::make_shared<chrome_axis::AccessoryProfile>();
                            device.discrete->push_back(binding);
                        }
                    }
                }
            }
            if (!inputConfigured)
                inputConfigured = input->configure(window, inputDevices);
            inputActive = true;
            {
                sampledAt = Clock::now();
                if (diagnostics && lastUsbPollFinished != Clock::time_point{} &&
                    sampledAt - lastUsbPollFinished > inputLifetime) {
                    diagnostics->recordUsbEvent({.sampledAt = GetTickCount64(),
                        .durationMicros = elapsedMicros(sampledAt - lastUsbPollFinished),
                        .kind = DiagnosticUsbEventKind::pollGap, .mode = DiagnosticMode::usb});
                }
                input->beginPoll(sampledAt);
                readAccessories = usb.accessories && vehicleInputFresh(current, sampledAt);
                if (usb.reader) {
                    controls = usb.reader->poll(*input, diagnostics ? &readings : nullptr);
                    if (diagnostics)
                        pedalRestReadings = usb.reader->pedalRestReadings();
                }
                if (readAccessories) {
                    std::bitset<256> interruptedKeys;
                    input->visitAccessoryDisruptions([&](std::string_view id, bool reset) {
                        if (reset)
                            usb.accessories->resetDiscrete(id);
                        for (const auto& binding : *usb.accessoryProfile) {
                            if (binding.deviceId == id && binding.key)
                                interruptedKeys.set(binding.key);
                        }
                    });
                    if (interruptedKeys.any()) {
                        previousHeldKeys &= ~interruptedKeys;
                        accessoryKeys.releaseBindings(interruptedKeys);
                    }
                    if (accessoryPresses.pending())
                        accessoryPresses.keep(usb.accessories->armedKeyboardBindings(), sampledAt);
                    std::bitset<256> transientHolds;
                    chrome_axis::AccessoryKeyTimes holdStarted;
                    bool keyOverflow = false;
                    const auto addKey = [&](unsigned int key, Clock::time_point when, bool tap) {
                        if (!keyOverflow)
                            keyOverflow = !accessoryPresses.push(key, when, tap);
                    };
                    input->replayAccessories([&](bool latest) {
                        std::bitset<256> pulses;
                        const auto observed = latest && diagnostics
                            ? std::span<chrome_axis::AccessoryObservation>{accessoryObservations}
                            : std::span<chrome_axis::AccessoryObservation>{};
                        const auto held = usb.accessories->poll(*input, observed, &pulses,
                            latest ? &handbrake : nullptr, latest ? &nativeGear : nullptr,
                            &transmissionSample, &keyTimes, !latest);
                        input->observePaddles(paddleBatch, transmissionSample, inputLifetime);
                        const auto released = transientHolds & ~held;
                        const auto started = latest ? std::bitset<256>{} : held & ~previousHeldKeys;
                        replayReleases |= previousHeldKeys & ~held;
                        if (pulses.any() || released.any() || started.any()) {
                            for (const auto& binding : *usb.accessoryProfile) {
                                const auto key = binding.key;
                                if (pulses.test(key))
                                    addKey(key, keyTimes[key], false);
                                if (released.test(key))
                                    addKey(key, holdStarted[key], true);
                                if (started.test(key))
                                    holdStarted[key] = keyTimes[key];
                            }
                        }
                        transientHolds = (transientHolds & held) | started;
                        previousHeldKeys = held;
                        if (latest)
                            requestedKeys = held;
                    });
                    paddleBatch.finish();
                    paddleOverflow = paddleBatch.overflowed();
                    if (accessoryPresses.pending())
                        accessoryPresses.keep(usb.accessories->armedKeyboardBindings(), sampledAt);
                    if (keyOverflow) {
                        accessoryPresses.clear();
                        requestedKeys.reset();
                        previousHeldKeys.reset();
                        usb.accessories->resetKeyboardBindings();
                        if (usbReadDiagnostics)
                            usbReadDiagnostics->record({chrome_axis::DirectInputReadEventKind::discreteOverflow,
                                {}, "USB keyboard transitions"}, GetTickCount64());
                    }
                } else if (usb.accessories) {
                    usb.accessories->reset();
                    input->discardAccessorySamples();
                    previousHeldKeys.reset();
                    accessoryPresses.clear();
                }
                lastUsbPollFinished = Clock::now();
                const auto duration = lastUsbPollFinished - sampledAt;
                sampleExpired = duration > inputLifetime;
                if (sampleExpired && diagnostics) {
                    const auto tick = GetTickCount64();
                    diagnostics->recordUsbEvent({.sampledAt = tick, .durationMicros = elapsedMicros(duration),
                        .kind = DiagnosticUsbEventKind::pollExpired, .mode = DiagnosticMode::usb});
                    lastUsbReadiness.reset();
                    if (!usbWaitStartedAt)
                        usbWaitStartedAt = tick;
                }
            }
            input->drainCycles([&](const auto& cycle) {
                if (measuring) {
                    performance.usbRead.add(cycle.durationMicros);
                    performance.usbSlowReads += cycle.durationMicros > elapsedMicros(physicalPollInterval);
                    performance.usbLateStarts += cycle.missedCadence;
                    performance.usbRetryWaits += cycle.retryAfterUnavailable;
                }
            });
            if (sampleExpired) {
                usb.resetReaders();
                controls = {};
                readings = {};
                requestedKeys.reset();
                previousHeldKeys.reset();
                accessoryPresses.clear();
                input->discardAccessorySamples();
                paddleBatch = {};
                paddleOverflow = false;
                handbrake.reset();
                nativeGear.reset();
                transmissionSample = {};
            }
        }
        const bool hasRequestedKeys = requestedKeys.any() || accessoryPresses.pending();
        const bool keyboardOutputPending = hasRequestedKeys || accessoryKeys.hasInjectedKeys();
        const bool accessoryContextRequired = keyboardOutputPending || handbrake.has_value() || !paddleBatch.edges().empty();
        bool accessoryContextLive = false;
        if (accessoryContextRequired) {
            std::scoped_lock lock(stateMutex);
            const auto now = Clock::now();
            accessoryContextLive = context.generation == current.generation && context.vehicle == current.vehicle &&
                vehicleInputFresh(context, now) && now - context.refreshed <= contextLifetime &&
                now - sampledAt <= inputLifetime && !stop.stop_requested();
        }
        const bool accessoryContextLost = accessoryContextRequired && !accessoryContextLive;
        const bool blockedByModifier = keyboardOutputPending && !accessoryContextLost && modifierDown();
        const bool accessoryBlocked = keyboardOutputPending && (accessoryContextLost || blockedByModifier);
        if (blockedByModifier)
            modifierBlockedUntil = start + diagnosticIssueDuration;
        if (accessoryContextLost) {
            handbrake.reset();
            nativeGear.reset();
            transmissionSample = {};
            paddleBatch = {};
            paddleOverflow = false;
            previousHeldKeys.reset();
            if (input)
                input->discardAccessorySamples();
            if (usb.accessories)
                usb.accessories->reset();
            readAccessories = false;
        } else if (blockedByModifier && usb.accessories) {
            usb.accessories->resetKeyboardBindings();
            previousHeldKeys.reset();
        }

        const bool axesReady = usb.reader && std::ranges::all_of(usb.reader->axisStatuses(), [](chrome_axis::AxisStatus status) {
            return status == chrome_axis::AxisStatus::armed || status == chrome_axis::AxisStatus::unassigned;
        });
        const bool selectorUsable = !usb.nativeShifter || (usb.nativeShifterVerified && nativeGear.has_value());
        const auto readiness = physical && usb.accessories
            ? usb.accessories->readiness(selectorUsable) : chrome_axis::AccessoryReadiness{};
        const auto hasAxisStatus = [&](chrome_axis::AxisStatus status) {
            return usb.reader && std::ranges::find(usb.reader->axisStatuses(), status) != usb.reader->axisStatuses().end();
        };
        const bool pendingRead = readiness.temporarilyUnavailable ||
            hasAxisStatus(chrome_axis::AxisStatus::temporarilyUnavailable);
        if (measuring && physical) {
            performance.expiredReads += sampleExpired;
            performance.transientReads += pendingRead;
        }
        const bool accessoriesReady = usb.accessoryProfile && !usb.accessoryProfile->empty() &&
            readiness.armed == usb.accessoryProfile->size();
        const bool usbReady = physical && usb.profilesValid() && (!usb.reader || axesReady) &&
            (!usb.accessories || accessoriesReady);
        const auto accessoryState = [&](bool previouslyBlocked) {
            if (!sampleExpired && transmissionSample.selectorAmbiguous)
                return DiagnosticAccessoryState::selectorAmbiguous;
            if (!sampleExpired && !accessoriesReady && start < modifierBlockedUntil)
                return DiagnosticAccessoryState::modifierBlocked;
            return diagnosticAccessoryState(readiness, accessoriesReady, input && input->ready(),
                                            sampleExpired, !sampleExpired && (accessoryBlocked || previouslyBlocked));
        };
        const auto commandState = [&](std::size_t index) {
            if (sampleExpired)
                return DiagnosticCommandState::sampleExpired;
            if (!input || !input->ready())
                return DiagnosticCommandState::deviceUnavailable;
            if (!readAccessories)
                return DiagnosticCommandState::waitingForVehicle;
            const auto& binding = (*usb.accessoryProfile)[index];
            const auto state = diagnosticCommandState(accessoryObservations[index].status);
            if (state == DiagnosticCommandState::ready &&
                ((binding.nativeShifter && !selectorUsable) || (binding.key && blockedByModifier)))
                return DiagnosticCommandState::waitingForNeutral;
            return state;
        };
        if (diagnostics && physical && !sampleExpired && vehicleInputFresh(current, start)) {
            if (!usbReady || !lastUsbReadiness || lastUsbReadiness->kind != DiagnosticUsbEventKind::commandsReady) {
                const auto tick = GetTickCount64();
                DiagnosticUsbEvent event{.sampledAt = tick,
                    .durationMicros = usbReady && usbWaitStartedAt ? (tick - usbWaitStartedAt) * 1000 : 0,
                    .kind = usbReady ? DiagnosticUsbEventKind::commandsReady : DiagnosticUsbEventKind::commandsWaiting,
                    .mode = DiagnosticMode::usb};
                if (usb.reader) {
                    const auto& axes = usb.reader->axisStatuses();
                    for (std::size_t i = 0; i < axes.size(); ++i)
                        event.axes[i] = diagnosticAxisState(axes[i], false);
                }
                if (usb.accessories) {
                    event.accessories = accessoryState(false);
                    event.accessoryArmed = static_cast<std::uint32_t>(readiness.armed);
                    event.accessoryCount = static_cast<std::uint32_t>(usb.accessoryProfile->size());
                    for (std::size_t i = 0; i < usb.accessoryProfile->size() && !accessoriesReady; ++i) {
                        const auto& binding = (*usb.accessoryProfile)[i];
                        const auto state = commandState(i);
                        if (state == DiagnosticCommandState::ready)
                            continue;
                        if (event.blockerCount < event.blockers.size()) {
                            event.blockers[event.blockerCount] = {
                                binding.kind, binding.role, static_cast<std::uint32_t>(binding.index), binding.key, state,
                                binding.clutchMode.has_value(), binding.handbrakeCeilingPercent != 0};
                        }
                        ++event.blockerCount;
                    }
                }
                if (!lastUsbReadiness || lastUsbReadiness->kind != event.kind ||
                    lastUsbReadiness->axes != event.axes || lastUsbReadiness->accessories != event.accessories ||
                    lastUsbReadiness->accessoryArmed != event.accessoryArmed ||
                    lastUsbReadiness->blockerCount != event.blockerCount || lastUsbReadiness->blockers != event.blockers) {
                    diagnostics->recordUsbEvent(event);
                    lastUsbReadiness = event;
                }
                if (usbReady)
                    usbWaitStartedAt = 0;
                else if (!usbWaitStartedAt)
                    usbWaitStartedAt = tick;
            }
        } else if (!sampleExpired) {
            lastUsbReadiness.reset();
            usbWaitStartedAt = 0;
        }
        const bool usbCanSend = controls.steeringReady || controls.throttleReady || controls.brakeReady ||
            readiness.armed != 0;
        auto hudStatus = HudStatus::loaded;
        if (simulated) {
            hudStatus = HudStatus::simulation;
        } else if (usb.enabled) {
            hudStatus = usbReady && !sampleExpired ? HudStatus::usbReady
                : usbCanSend && !sampleExpired ? HudStatus::usbPartial : HudStatus::usbWaiting;
        }
        const OutputContext output{current.vehicle, current.generation, sampledAt};
        ForceOutputContext forceOutput{output, {}, {}};
        std::optional<chrome_axis::FeedbackSample> forceSample;
        {
            std::scoped_lock lock(stateMutex);
            // Timestamp from before polling so a stalled driver cannot publish a fresh-looking frame.
            if (context.generation == current.generation) {
                if (hudFrame.status != hudStatus || !usbCanSend) {
                    lastUsbSignal = {};
                }
                hudFrame = {hudStatus, current.generation, sampledAt};
                const bool nativeShifter = !simulated && usb.nativeShifter;
                const bool nativeSequential = !simulated && usb.nativeSequential;
                const auto clutchMode = simulated ? std::nullopt : usb.clutchMode;
                if (!nativeSequential || !transmissionSample.sequentialArmed) {
                    paddleRequests.clear();
                } else {
                    for (const auto& [delta, timestamp] : paddleBatch.edges()) {
                        if (sourceInputFresh(timestamp, sampledAt))
                            paddleOverflow |= !paddleRequests.push(delta, timestamp);
                    }
                    if (paddleOverflow) {
                        paddleRequests.discard();
                        transmissionSample.sequentialArmed = false;
                        usb.accessories->resetSequential();
                    }
                }
                if (simulated || usb.reader || handbrake || nativeShifter || nativeSequential || clutchMode) {
                    inputFrame = {controls, readings, current.generation, sampledAt, simulated, handbrake,
                                  nativeShifter, nativeShifter && usb.nativeShifterVerified, nativeGear,
                                  nativeSequential, transmissionSample.sequentialArmed, clutchMode, transmissionSample.clutch,
                                  transmissionSample.selectorAmbiguous, transmissionSample.handbrakeSampledAt,
                                  transmissionSample.gearSampledAt, transmissionSample.sequentialSampledAt,
                                  transmissionSample.clutchSampledAt, transmissionSample.clutchObservation};
                } else {
                    inputFrame = {};
                }
                if (wantFeedback && controls.steeringReady) {
                    const auto now = Clock::now();
                    if (context.vehicleUpdated != Clock::time_point{} && vehicleMotion.sampled != Clock::time_point{} &&
                        now - context.vehicleUpdated <= inputLifetime && now - context.refreshed <= contextLifetime &&
                        now - sampledAt <= inputLifetime && now - vehicleMotion.sampled <= motionLifetime) {
                        forceOutput.motionSampled = vehicleMotion.sampled;
                        forceOutput.vehicleUpdated = context.vehicleUpdated;
                        forceOutput.input.sampled = controls.sampledAt[0];
                        const auto eventSeparation = vehicleMotion.impacted - vehicleMotion.landed;
                        const bool landingHasImpact = usb.feedbackSettings.impacts &&
                            eventSeparation > -80ms && eventSeparation < 80ms;
                        forceSample = chrome_axis::FeedbackSample{
                            .steering = controls.calibratedSteering,
                            .forwardSpeed = vehicleMotion.forwardSpeed,
                            .sideSpeed = vehicleMotion.sideSpeed,
                            .mass = vehicleMotion.mass,
                            .impactSpeed = vehicleMotion.impactSpeed,
                            .impactSide = vehicleMotion.impactSide,
                            .impactAge = std::chrono::duration<float>(now - vehicleMotion.impacted).count(),
                            .lateralAcceleration = vehicleMotion.lateralAcceleration,
                            .yawRate = vehicleMotion.yawRate,
                            .landingSpeed = landingHasImpact ? 0.f : vehicleMotion.landingSpeed,
                            .landingAge = std::chrono::duration<float>(now - vehicleMotion.landed).count(),
                            .skidding = vehicleMotion.skidding,
                            .airborne = vehicleMotion.airborne,
                        };
                    }
                }
            }
        }
        if (paddleOverflow && usbReadDiagnostics)
            usbReadDiagnostics->record({chrome_axis::DirectInputReadEventKind::discreteOverflow,
                {}, "USB paddle requests"}, GetTickCount64());
        AccessoryDelivery delivery;
        if (usb.accessories || accessoryKeys.hasInjectedKeys()) {
            delivery = accessoryKeys.apply(requestedKeys, &accessoryPresses,
                accessoryBlocked || sampleExpired ? nullptr : foreground, output, &keyTimes, replayReleases);
            const auto events = delivery.pressed | delivery.blocked | delivery.failed;
            if (events.any()) {
                std::scoped_lock lock(stateMutex);
                if (context.generation == current.generation) {
                    const auto sentAt = delivery.key ? GetTickCount64() : 0;
                    const auto now = Clock::now();
                    if (usb.accessoryProfile) {
                        for (const auto& binding : *usb.accessoryProfile) {
                            if (!binding.key)
                                continue;
                            const auto key = binding.key;
                            auto& history = accessoryHistory[key];
                            if (delivery.pressed.test(key)) {
                                history = {sentAt};
                            } else if (delivery.failed.test(key) || delivery.blocked.test(key)) {
                                history.issue = delivery.failed.test(key)
                                    ? DiagnosticCommandOutput::failed : DiagnosticCommandOutput::blocked;
                                history.issueUntil = now + diagnosticIssueDuration;
                            }
                        }
                    }
                    if (delivery.key) {
                        diagnostic.lastAccessoryKey = delivery.key;
                        diagnostic.accessorySentAt = sentAt;
                        if (usbHudStatus(hudFrame.status))
                            lastUsbSignal = now;
                    }
                }
            }
        }
        auto& forceDiagnostic = diagnostic.feedbackData;
        if (!forceSample || stop.stop_requested() || !forceOutput.current()) {
            if (lastFeedbackUpdate != Clock::time_point{})
                resetFeedback();
        } else if (lastFeedbackUpdate == Clock::time_point{} || start - lastFeedbackUpdate >= feedbackInterval) {
            const float seconds = lastFeedbackUpdate == Clock::time_point{} ? 0.020f
                : std::chrono::duration<float>(start - lastFeedbackUpdate).count();
            const bool missedCadence = lastFeedbackUpdate != Clock::time_point{} &&
                start - lastFeedbackUpdate > feedbackInterval + physicalPollInterval;
            lastFeedbackUpdate = start;
            const auto& steering = usb.profile->steering;
            const float direction = steering.positive > steering.neutral ? 1.0f : -1.0f;
            forceDiagnostic.motionAgeMicros = elapsedMicros(Clock::now() - forceOutput.motionSampled);
            forceDiagnostic.motionAvailable = 1;
            forceDiagnostic.airborne = forceSample->airborne;
            forceDiagnostic.skidding = forceSample->skidding;
            const auto effect = wheelFeedback.update(usb.feedbackSettings, *forceSample, seconds);
            const float force = direction * effect.force;
            const bool clipped = effect.saturation != chrome_axis::FeedbackSaturation::none;
            const bool impactClipped = effect.saturation == chrome_axis::FeedbackSaturation::transient;
            forceDiagnostic.rawRequested = direction * effect.requestedForce;
            forceDiagnostic.requested = force;
            forceDiagnostic.requestAvailable = 1;
            forceDiagnostic.clipped = clipped;
            forceDiagnostic.impactClipped = impactClipped;
            const auto connection = input ? input->connectionToken(steering.deviceId) : 0;
            if (feedbackConnection != connection) {
                // A result from the old connection cannot become the first accepted effect of the new one.
                feedbackOutput.cancel();
                feedbackConnection = connection;
                firstForceRequestedAt = 0;
                firstEffectAccepted = false;
            }
            const auto requestedAt = GetTickCount64();
            if (!feedbackOutput.submit(foreground, steering.deviceId, steering.axis, force, forceOutput,
                                       measuring ? performance.requestId : 0, missedCadence, connection)) {
                feedbackState = chrome_axis::WheelForceState::unavailable;
            } else if (!firstForceRequestedAt && std::abs(force) >= 0.001f) {
                firstForceRequestedAt = requestedAt;
            }
            if (measuring) {
                performance.motionAge.add(forceDiagnostic.motionAgeMicros);
                feedbackMeasurement.identify(performance, usb.feedbackSettings, *usb.profile, diagnostic);
                ++performance.forceSamples;
                performance.peakContinuousForce = std::max(performance.peakContinuousForce, std::abs(effect.continuousForce));
                performance.clippedSamples += clipped;
                performance.impactClippedSamples += impactClipped;
                performance.peakRequestedForce = std::max(performance.peakRequestedForce, std::abs(effect.requestedForce));
                performance.peakLimitedForce = std::max(performance.peakLimitedForce, std::abs(force));
            }
        }
        collectFeedback();
        collectInputEvents();
        if (!simulated && usb.reader && input && input->ready()) {
            const auto& statuses = usb.reader->axisStatuses();
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
                    case AxisStatus::awaitingFirstReport: return "waiting for first report";
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
            if (accessoryBlocked || delivery.blocked.any() || delivery.failed.any())
                accessoryBlockedUntil = now + diagnosticIssueDuration;
        }
        // Capture brief interruptions that fall between regular diagnostic snapshots.
        // A slow effect submission does not change whether the earlier input read succeeded.
        const auto diagnosticAt = Clock::now();
        const bool reportPending = diagnostics && physical && diagnosticAt >= nextPendingReport &&
            (pendingRead || readiness.awaitingFirstReport || hasAxisStatus(chrome_axis::AxisStatus::awaitingFirstReport));
        if (diagnostics && (changes.feedback || diagnosticAt >= nextDiagnostic || (sampleExpired && diagnosticAt >= nextTimeoutReport) ||
            reportPending)) {
            if (sampleExpired)
                nextTimeoutReport = diagnosticAt + diagnosticIssueDuration;
            if (reportPending)
                nextPendingReport = diagnosticAt + diagnosticIssueDuration;
            diagnostic.mode = diagnosticSource;
            diagnostic.profileLoaded = physical && usb.hasProfileDirectory();
            for (std::size_t i = 0; i < diagnostic.axes.size(); ++i) {
                auto& axis = diagnostic.axes[i];
                axis.state = simulated ? DiagnosticAxisState::simulated
                    : !usb.enabled ? DiagnosticAxisState::inactive
                    : !usb.reader ? DiagnosticAxisState::awaitingProfile
                    : diagnosticAxisState(usb.reader->axisStatuses()[i], sampleExpired);
                axis.inputValue = readings[i].value_or(0.0f);
                axis.inputAvailable = readings[i].has_value() && !sampleExpired;
                axis.pedalRest = i > 0 && axis.state == DiagnosticAxisState::waitingForNeutral
                    ? pedalRestReadings[i - 1] : chrome_axis::PedalRestReading{};
            }
            diagnostic.accessoryCount = physical && usb.accessoryProfile
                ? static_cast<std::uint32_t>(usb.accessoryProfile->size()) : 0;
            diagnostic.accessoryArmed = 0;
            diagnostic.accessories = simulated || !usb.enabled ? DiagnosticAccessoryState::inactive
                : usb.hasAccessoryError() ? DiagnosticAccessoryState::profileError : DiagnosticAccessoryState::unconfigured;
            if (physical && usb.accessories) {
                diagnostic.accessoryArmed = static_cast<std::uint32_t>(readiness.armed);
                diagnostic.accessories = accessoryState(start < accessoryBlockedUntil);
            }
            if (!simulated && usb.enabled && usb.accessoryAxisConflict)
                diagnostic.accessories = DiagnosticAccessoryState::axisConflict;
            for (std::size_t i = 0; i < diagnostic.accessoryCount; ++i) {
                const auto& binding = (*usb.accessoryProfile)[i];
                const auto key = binding.key;
                auto& row = diagnostic.accessoryCommands[i];
                row = {.kind = binding.kind, .role = binding.role, .activation = binding.activation,
                       .index = static_cast<std::uint32_t>(binding.index), .key = key,
                       .rest = binding.rest, .engaged = binding.engaged,
                       .handbrakeCeilingPercent = binding.handbrakeCeilingPercent};
                const bool keyboard = binding.key != 0;
                if (keyboard) {
                    const auto& history = accessoryHistory[key];
                    row.lastSentAt = history.sentAt;
                    if (start < history.issueUntil)
                        row.output = history.issue;
                    else if (delivery.pulsing.test(key))
                        row.output = DiagnosticCommandOutput::pulsing;
                    else if (delivery.held.test(key))
                        row.output = DiagnosticCommandOutput::held;
                }
                row.state = commandState(i);
                if (readAccessories && !sampleExpired) {
                    const auto& observation = accessoryObservations[i];
                    row.active = observation.engaged.value_or(false);
                    row.activeAvailable = observation.engaged.has_value();
                    row.travel = static_cast<float>(observation.travel.value_or(0.0));
                    row.travelAvailable = observation.travel.has_value();
                }
            }
            diagnostic.feedback = !simulated && usb.feedbackIssue ? *usb.feedbackIssue
                : simulated && usb.feedbackSettings.enabled ? DiagnosticFeedbackState::inactive
                : !usb.feedbackSettings.enabled ? DiagnosticFeedbackState::disabled
                : !wantFeedback ? DiagnosticFeedbackState::unavailable
                : diagnosticFeedbackState(feedbackState, controls.steeringReady, forceDiagnostic.motionAvailable != 0);
            publishDiagnostics(foreground, current);
        }
        const auto interval = physical ? physicalPollInterval : simulated ? simulationPollInterval : idlePollInterval;
        std::this_thread::sleep_until(start + interval);
    }
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
        chrome_axis::setTransmissionHotkeyContext(false);
        suspendTransmission();
        resetSimulator();
        vehicleDelivery = {};
        return;
    }
    // A report can arrive just before the game hook resumes after a vehicle-input gap.
    // Invalidate it so the worker waits for neutral before using the lever again.
    const bool resumedWithLever = (inputFrame.handbrake || inputFrame.nativeShifter || inputFrame.nativeSequential ||
        inputFrame.clutchMode) && !vehicleInputFresh(context, now);
    const bool captureTelemetry = inputAgeCapture.started != Clock::time_point{};
    context.vehicleUpdated = now;
    if (resumedWithLever) {
        suspendTransmission();
        advanceDrivingGeneration();
        invalidateFrames();
        return;
    }
    if (inputFrame.generation != context.generation || now - inputFrame.sampled > inputLifetime) {
        const bool captureForeground = captureTelemetry && foregroundGameWindow();
        chrome_axis::setTransmissionHotkeyContext(false);
        suspendTransmission(captureForeground ? vehicle : nullptr);
        resetSimulator();
        vehicleDelivery = {};
        if (captureForeground) {
            if (!transmissionPedals.tracking() && std::isfinite(vehicle->acceleration) && std::isfinite(vehicle->deceleration)) {
                vehicleDelivery.sampled = now;
                vehicleDelivery.pedalTick = GetTickCount64();
                vehicleDelivery.finalPedals = {vehicle->acceleration, vehicle->deceleration};
            }
        }
        return;
    }
    if (!foregroundGameWindow()) {
        suspendTransmission();
        advanceDrivingGeneration();
        context.vehicleUpdated = {};
        invalidateFrames();
        return;
    }

    auto controls = inputFrame.state;
    auto appliedReadings = inputFrame.readings;
    const std::array ready{&controls.steeringReady, &controls.throttleReady, &controls.brakeReady};
    const std::array values{&controls.values.steering, &controls.values.throttle, &controls.values.brake};
    for (std::size_t i = 0; i < controls.sampledAt.size(); ++i) {
        if (!sourceInputFresh(controls.sampledAt[i], now)) {
            *ready[i] = false;
            *values[i] = 0;
            appliedReadings[i].reset();
        }
    }
    const bool simulated = inputFrame.simulated;
    const auto hotkey = transmissionAvailable ? chrome_axis::transmissionHotkeyBinding() : 0;
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
        const auto simulatedValues = simulator.update(chrome_axis::simulationKeys(hotkey), seconds);
        controls = {simulatedValues, simulatedValues.steering != 0.0f,
                    simulatedValues.throttle > 0.0f, simulatedValues.brake > 0.0f};
    }
    const chrome_axis::PedalRelease pedalsReleased{
        (simulated || !controls.assigned[1] || controls.throttleReady) && controls.values.throttle == 0.0f,
        (simulated || !controls.assigned[2] || controls.brakeReady) && controls.values.brake == 0.0f,
    };
    const auto pedalDelivery = updateTransmission(vehicle, pedalsReleased, hotkey, now, captureTelemetry);
    const auto routing = pedalDelivery.routing;
    const bool reversePedals = routing == chrome_axis::PedalRouting::reverse;
    controls.throttleReady &= pedalDelivery.throttleAllowed;
    controls.brakeReady &= pedalDelivery.brakeAllowed;

    const auto requestedHandbrake = sourceInputFresh(inputFrame.handbrakeSampledAt, now) ? inputFrame.handbrake : std::nullopt;
    std::optional<HandbrakeDelivery> handbrake;
    bool handbrakeWritten = false;
    if (requestedHandbrake) {
        const float native = readField(*vehicle, handbrakeOffset);
        if (std::isfinite(native) && native >= 0.0f && native <= 1.0f &&
            std::isfinite(*requestedHandbrake) && *requestedHandbrake >= 0.0f && *requestedHandbrake <= 1.0f) {
            const float combined = std::max(native, *requestedHandbrake);
            handbrake = HandbrakeDelivery{*requestedHandbrake, combined};
            handbrakeWritten = combined > native;
            if (handbrakeWritten)
                writeField(*vehicle, handbrakeOffset, combined);
        }
    }

    auto attenuation = std::optional<float>{};
    if (probePosition && routing == chrome_axis::PedalRouting::standard && pedalDelivery.throttleAllowed) {
        const auto gearbox = manual_backend::snapshot(vehicle);
        if (gearbox.manualActive && manual_backend::detail::forwardGear(gearbox.engagedGear, gearbox.gearCount) &&
            manual_backend::detail::forwardGear(gearbox.targetGear, gearbox.gearCount)) {
            attenuation = *probePosition;
        }
    }
    // Missing or stale throttle samples are zero-filled. Only a valid release rearms the pedal.
    probeThrottleInterlock = chrome_axis::clutchThrottleInterlock(probeThrottleInterlock, attenuation,
                                                                 pedalsReleased.throttle);
    if (probeThrottleInterlock && !attenuation)
        controls.throttleReady = false;

    const bool axesRequested = controls.steeringReady || controls.throttleReady || controls.brakeReady;
    const bool readAxes = axesRequested || attenuation || captureTelemetry;
    const float gameSteering = readAxes ? readField(*vehicle, steeringOffset) : 0.0f;
    // In reverse, Cyberpunk uses the deceleration channel for propulsion and the
    // acceleration channel for service braking. Keep the configured pedals logical.
    const float gameThrottle = readAxes ? (reversePedals ? vehicle->deceleration : vehicle->acceleration) : 0.0f;
    const float gameBrake = readAxes ? (reversePedals ? vehicle->acceleration : vehicle->deceleration) : 0.0f;
    const bool axesAvailable = readAxes && std::isfinite(gameSteering) && std::isfinite(gameThrottle) &&
        std::isfinite(gameBrake);
    if (!axesAvailable) {
        if (!handbrake) {
            vehicleDelivery = {};
            return;
        }
        controls = {};
        attenuation.reset();
    }

    const float steering = std::clamp(controls.values.steering, -1.0f, 1.0f);
    // Invert the quadratic steering response observed by the upstream mod.
    const float compensated = std::copysign(std::sqrt(std::abs(steering)), steering);
    const bool steer = controls.steeringReady && std::abs(compensated) > std::abs(gameSteering);
    const float combinedThrottle = controls.throttleReady ? std::max(gameThrottle, controls.values.throttle) : gameThrottle;
    // Explicit probe only. This changes the verified accelerator channel, not the game's clutch.
    const float throttle = attenuation ? combinedThrottle * (1.0f - *attenuation) : combinedThrottle;
    const float brake = controls.brakeReady ? std::max(gameBrake, controls.values.brake) : gameBrake;
    const bool throttleWritten = (controls.throttleReady || attenuation) && throttle != gameThrottle;
    const bool brakeWritten = controls.brakeReady && brake != gameBrake;
    const bool pedals = throttleWritten || brakeWritten;
    const auto delivery = [&](std::size_t axis, float value, bool ready, bool applied) {
        return AxisDelivery{appliedReadings[axis], value, !ready ? DiagnosticDelivery::unavailable
            : applied ? DiagnosticDelivery::applied : DiagnosticDelivery::gameKept};
    };
    const auto tick = GetTickCount64();
    const bool hasPedals = axesAvailable && routing != chrome_axis::PedalRouting::suspended &&
        (!simulated || captureTelemetry);
    vehicleDelivery = {{delivery(0, compensated, controls.steeringReady, steer),
                         delivery(1, attenuation ? throttle : controls.values.throttle,
                                  controls.throttleReady || attenuation.has_value(), throttleWritten),
                         delivery(2, controls.values.brake, controls.brakeReady, brakeWritten)}, now,
                         {throttle, brake}, hasPedals ? tick : 0, handbrake, attenuation};
    if (steer) {
        writeField(*vehicle, steeringOffset, compensated);
    }
    if (pedals) {
        vehicle->acceleration = reversePedals ? brake : throttle;
        vehicle->deceleration = reversePedals ? throttle : brake;
        const float drive = vehicle->acceleration - vehicle->deceleration;
        writeField(*vehicle, driveOffset, drive);
        writeField(*vehicle, secondaryDriveOffset, drive);
    }
    const bool applied = steer || pedals || handbrakeWritten;
    if (applied) {
        if (!simulated && inputAgeCapture.started != Clock::time_point{}) {
            auto oldestApplied = inputFrame.sampled;
            const auto include = [&](Clock::time_point observed) {
                if (observed != Clock::time_point{})
                    oldestApplied = std::min(oldestApplied, observed);
            };
            if (steer)
                include(controls.sampledAt[0]);
            if (throttleWritten)
                include(controls.sampledAt[1]);
            if (brakeWritten)
                include(controls.sampledAt[2]);
            if (handbrakeWritten)
                include(inputFrame.handbrakeSampledAt);
            if (oldestApplied >= inputAgeCapture.started)
                inputAgeCapture.timing.add(elapsedMicros(Clock::now() - oldestApplied));
        }
        if (steer)
            appliedAxes[0] = {compensated, tick};
        if (throttleWritten)
            appliedAxes[1] = {throttle, tick};
        if (brakeWritten)
            appliedAxes[2] = {brake, tick};
        if (!simulated && usbHudStatus(hudFrame.status))
            lastUsbSignal = now;
    }
    lock.unlock();

    if (applied) {
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
    {
        std::scoped_lock lock(driverLogMutex);
        driverLoggingEnabled = false;
    }
    accessoryKeys.apply({});
    if (readerThread.joinable()) {
        readerThread.request_stop();
        readerThread.join();
    }
    if (driverCleanup) {
        driverCleanup->release();
        if (!driverCleanup->wait(1000))
            warn("USB cleanup is still waiting for a DirectInput call. Continuing game shutdown.");
        driverCleanup.reset();
    }
    transmissionAvailable = false;
    chrome_axis::stopTransmissionHotkey();
    manual_backend::detach();
    if (hookTarget) {
        if (sdk->hooking->Detach(plugin, hookTarget)) {
            hookTarget = nullptr;
        } else {
            warn("RED4ext did not confirm hook detachment.");
        }
    }
    std::scoped_lock lock(stateMutex);
    context = {};
    transmissionInput = {};
    transmissionShortcut = {};
    transmissionPedals = {};
    appliedAxes = {};
    inputAgeCapture = {};
    drivingCaptureActive.store(false, std::memory_order_relaxed);
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

    driverCleanup = std::make_unique<chrome_axis::DeferredDriverCleanup<DriverWorkers>>();
    {
        std::scoped_lock lock(driverLogMutex);
        driverLoggingEnabled = true;
    }
    auto* workers = &driverCleanup->workers();
    readerThread = std::jthread([configurationDirectory, workers](std::stop_token stop) {
        try {
            pollInput(stop, configurationDirectory, *workers);
        } catch (const std::exception& failure) {
            feedbackRequested.store(false, std::memory_order_relaxed);
            accessoryKeys.apply({});
            warn(failure.what());
            warn("Chrome Axis input reader stopped. Vanilla input remains available.");
            std::scoped_lock lock(stateMutex);
            inputAgeCapture = {};
            drivingCaptureActive.store(false, std::memory_order_relaxed);
            invalidateFrames();
        }
    });
    hudStorageGameRoot = chrome_axis::installedGameRoot(directory);
    if (!sdk->scripts->Add(plugin, script.c_str())) {
        warn("RED4ext could not register the driving context script. Analog input disabled.");
        return false;
    }
    rtti->AddPostRegisterCallback(&registerNative);
    transmissionAvailable = manual_backend::attach(plugin, sdk);
    if (!transmissionAvailable)
        warn("Native transmission unavailable. Standard controls remain available.");
    else if (!chrome_axis::startTransmissionHotkey(plugin))
        warn("The AUTO/MANUAL keyboard listener failed to start. Using key polling where available.");
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
