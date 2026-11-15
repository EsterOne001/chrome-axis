#include "manual_probe.hpp"

#include <windows.h>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/vehicleBaseObject.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <system_error>

#include "backend.hpp"
#include "shift_request.hpp"

namespace manual_probe {
namespace {
using Clock = std::chrono::steady_clock;
using Vehicle = RED4ext::vehicle::BaseObject;
using namespace std::chrono_literals;

constexpr auto markerPollInterval = 250ms;
constexpr auto maximumSampleGap = 450ms;
constexpr auto sessionTimeout = 20min;
constexpr float minimumMotionSpeed = 2.0f;
constexpr double metresPerSecondToMph = 2.2369363;

struct Probe {
    std::mutex mutex;
    std::filesystem::path markerPath;
    std::filesystem::path statusPath;
    std::filesystem::path csvPath;
    std::FILE* csv{};
    Clock::time_point nextMarkerPoll{};
    Clock::time_point started{};
    Clock::time_point lastHeartbeat{};
    Clock::time_point lastSpeedTime{};
    Clock::time_point lastFlush{};
    Vehicle* vehicle{};
    Vehicle* referenceVehicle{};
    unsigned int session{};
    unsigned int segment{};
    int lastEngaged{-1};
    float lastSpeed{-1.0f};
    chrome_axis::ShiftKeys keys;
    bool initialized{};
    bool markerPresent{};
    bool recording{};
    bool toggleReleased{};
    bool toggleDown{};
    bool manual{};
};

Probe probe;

void setStatus(const char* message) noexcept {
    std::FILE* file = nullptr;
    if (!probe.statusPath.empty() && _wfopen_s(&file, probe.statusPath.c_str(), L"wb") == 0) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}

void pause(const char* message) noexcept {
    manual_backend::publish(nullptr, false, 0);
    probe.manual = false;
    probe.keys = {};
    probe.toggleReleased = false;
    probe.toggleDown = false;
    probe.lastSpeed = -1.0f;
    probe.lastEngaged = -1;
    if (probe.vehicle) {
        probe.vehicle = nullptr;
        setStatus(message);
        if (probe.csv)
            std::fflush(probe.csv);
    }
}

void stop(const char* message) noexcept {
    pause(message);
    probe.recording = false;
    setStatus(message);
}

void start(Clock::time_point now) noexcept {
    if (!probe.csv) {
        if (_wfopen_s(&probe.csv, probe.csvPath.c_str(), L"wb") != 0) {
            stop("Unable to open trials.csv. Remove arm.txt and create it again after checking file access.");
            return;
        }
        if (std::fprintf(probe.csv, "session,segment,elapsed_s,event,manual,native_engaged_gear,native_target_gear,"
                                    "rpm,speed_mph,acceleration_mps2,native_gear_entries,requested_gear,"
                                    "native_requested_gear,shift_serial,native_outcome,suppressed_auto_shifts\n") < 0 ||
            std::fflush(probe.csv) != 0) {
            std::fclose(probe.csv);
            probe.csv = nullptr;
            stop("Unable to write trials.csv. Check file access and available space.");
            return;
        }
    }
    ++probe.session;
    probe.segment = 0;
    probe.referenceVehicle = nullptr;
    probe.started = now;
    probe.recording = true;
    setStatus("Armed. Enter the driver's seat. Record an automatic baseline before switching with F8.");
}

void pollMarker(Clock::time_point now) {
    if (now < probe.nextMarkerPoll)
        return;
    probe.nextMarkerPoll = now + markerPollInterval;
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(probe.markerPath, error);
    if (exists == probe.markerPresent)
        return;
    probe.markerPresent = exists;
    if (exists)
        start(now);
    else
        stop("Idle. Create arm.txt to start a new session.");
}

bool pressed(int key) noexcept {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool isLocallyControlled(Vehicle* vehicle) {
    auto* type = vehicle->GetType();
    auto* function = type ? type->GetFunction(RED4ext::CName{"IsAutoDriveModeEnabled"}) : nullptr;
    bool autoDrive = true;
    return function && function->flags.isNative && function->params.Size() == 0 &&
           RED4ext::ExecuteFunction(static_cast<void*>(vehicle), function, &autoDrive) && !autoDrive;
}

void writeSample(Clock::time_point now, const manual_backend::Snapshot& native, const char* event,
                 int requested, bool flushEvent) noexcept {
    const float speed = native.speedMetersPerSecond;
    char rpm[32]{};
    char mph[32]{};
    char acceleration[32]{};
    if (std::isfinite(native.engineRpm) && native.engineRpm >= 0.0f)
        std::snprintf(rpm, sizeof(rpm), "%.3f", static_cast<double>(native.engineRpm));
    if (std::isfinite(speed) && speed >= 0.0f && speed < 200.0f) {
        std::snprintf(mph, sizeof(mph), "%.3f", static_cast<double>(speed) * metresPerSecondToMph);
        const auto interval = std::chrono::duration<double>(now - probe.lastSpeedTime).count();
        if (probe.lastSpeed >= 0.0f && interval > 0.03 && interval < 0.40)
            std::snprintf(acceleration, sizeof(acceleration), "%.3f", (speed - probe.lastSpeed) / interval);
        probe.lastSpeed = speed;
        probe.lastSpeedTime = now;
    } else {
        probe.lastSpeed = -1.0f;
    }
    const auto elapsed = std::chrono::duration<double>(now - probe.started).count();
    const bool flush = now - probe.lastFlush >= 1s || flushEvent;
    if (flush)
        probe.lastFlush = now;
    if (std::fprintf(probe.csv, "%u,%u,%.3f,%s,%d,%d,%d,%s,%s,%s,%d,%d,%d,%u,%s,%u\n",
                     probe.session, probe.segment, elapsed, event, static_cast<int>(probe.manual),
                     native.engagedGear, native.targetGear, rpm, mph, acceleration, native.gearCount,
                     requested, native.requestedGear, native.shiftSerial, native.outcome,
                     native.suppressedAutoShifts) < 0 || (flush && std::fflush(probe.csv) != 0)) {
        stop("Unable to write trials.csv. Check file access and available space.");
    }
}

void sample(Vehicle* vehicle, Clock::time_point now) noexcept {
    if (!probe.vehicle || now - probe.lastHeartbeat > maximumSampleGap) {
        pause("Paused between driving samples.");
        probe.vehicle = vehicle;
        probe.referenceVehicle = vehicle;
        probe.lastHeartbeat = now;
        ++probe.segment;
        manual_backend::publish(vehicle, false, 0);
        setStatus("Waiting for native gearbox telemetry. F8 toggles manual in a forward gear.");
        return;
    }

    probe.lastHeartbeat = now;
    const auto native = manual_backend::snapshot();
    const bool toggle = pressed(VK_F8);
    const bool toggleEdge = probe.toggleReleased && toggle && !probe.toggleDown;
    probe.toggleReleased |= !toggle;
    probe.toggleDown = toggle;

    const char* event = native.engagedGear != probe.lastEngaged ? "native_gear_changed" : "sample";
    if (probe.manual && native.gearCount < 2) {
        probe.manual = false;
        probe.keys = {};
        event = "manual_lease_expired";
        setStatus("Manual paused. Drive in a forward gear and press F8 again.");
    } else if (toggleEdge) {
        probe.keys = {};
        if (probe.manual) {
            probe.manual = false;
            event = "manual_off";
            setStatus("Automatic restored. F8 enables experimental manual mode in a forward gear.");
        } else if (native.speedMetersPerSecond >= minimumMotionSpeed && native.engagedGear > 0 &&
                   native.engagedGear < native.gearCount) {
            probe.manual = true;
            event = "manual_on";
            setStatus("Manual active. Page Up / Page Down change gears. F8 restores automatic.");
        } else {
            event = "manual_unavailable";
            setStatus("Manual unavailable. Drive in a forward gear and press F8 again.");
        }
    } else if (native.gearCount > 1 && probe.lastEngaged == -1 && native.engagedGear >= 0 && !probe.manual) {
        setStatus("Recording native automatic gearbox. F8 toggles manual in a forward gear.");
    }

    const int delta = probe.manual ? probe.keys.update(pressed(VK_PRIOR), pressed(VK_NEXT)) : 0;
    const int requested = chrome_axis::requestedGear(native.engagedGear, native.gearCount, delta);
    if (delta != 0)
        event = requested > 0 ? (delta > 0 ? "request_up" : "request_down") : "outside_range";
    manual_backend::publish(vehicle, probe.manual, requested > 0 ? delta : 0);
    probe.lastEngaged = native.engagedGear;
    writeSample(now, native, event, requested, delta != 0 || toggleEdge);
}
} // namespace

void initialize(const std::filesystem::path& pluginDirectory, RED4ext::v1::PluginHandle plugin,
                const RED4ext::v1::Sdk* sdk) noexcept {
    std::scoped_lock lock(probe.mutex);
    try {
        const auto directory = pluginDirectory / L"manual-transmission-probe";
        std::filesystem::create_directories(directory);
        probe.statusPath = directory / L"status.txt";
        probe.markerPath = directory / L"arm.txt";
        probe.csvPath = directory / L"trials.csv";

        const auto legacyPlugin = pluginDirectory.parent_path() / L"ChromeAxisManualTransmissionProbe" /
                                  L"ChromeAxisManualTransmissionProbe.dll";
        if (std::filesystem::exists(legacyPlugin)) {
            setStatus("Manual probe disabled. Disable the original manual prototype and redeploy in Vortex.");
            return;
        }
        if (!manual_backend::attach(plugin, sdk)) {
            setStatus("Manual probe disabled. Unsupported gearbox layout or native hook unavailable.");
            return;
        }
        probe.initialized = true;
        setStatus("Idle. Create arm.txt to record the automatic gearbox and try the manual mode.");
    } catch (...) {
        setStatus("Manual probe disabled. Could not initialize its diagnostic directory.");
    }
}

void heartbeat(Vehicle* vehicle, bool foreground) noexcept {
    std::scoped_lock lock(probe.mutex);
    if (!probe.initialized)
        return;
    try {
        const auto now = Clock::now();
        pollMarker(now);
        if (!probe.recording)
            return;
        if (now - probe.started > sessionTimeout) {
            stop("Session finished. Remove arm.txt and create it again to rearm.");
            return;
        }
        if (!vehicle || !foreground || !isLocallyControlled(vehicle)) {
            pause("Paused: local vehicle controls unavailable. Release the keys before resuming.");
            return;
        }
        if (probe.referenceVehicle && probe.referenceVehicle != vehicle) {
            stop("Another vehicle was detected. Rearm to create a separate session.");
            return;
        }
        sample(vehicle, now);
    } catch (...) {
        stop("Manual probe paused after a diagnostic error. Remove arm.txt and create it again to rearm.");
    }
}

void shutdown() noexcept {
    std::scoped_lock lock(probe.mutex);
    manual_backend::detach();
    probe.initialized = false;
    if (probe.csv) {
        std::fclose(probe.csv);
        probe.csv = nullptr;
    }
}

} // namespace manual_probe