#include "chrome_axis/accessories.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/simulation.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace chrome_axis;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr auto device = "{11111111-1111-1111-1111-111111111111}";
constexpr auto preset = L"Profil \u00e9t\u00e9";
constexpr auto presetUtf8 = "Profil \xc3\xa9t\xc3\xa9";

fs::path pluginDirectory(const fs::path& game)
{
    return game / L"red4ext" / L"plugins" / L"chrome_axis";
}

template<class Check>
bool waitFor(Check check, std::chrono::seconds timeout = 5s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (check())
            return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

struct Fixture {
    fs::path directory;

    ~Fixture()
    {
        std::error_code error;
        fs::remove_all(directory, error);
    }
};

class Child {
public:
    explicit Child(const fs::path& directory) : directory_(directory) {}

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    ~Child()
    {
        if (handle_)
            finish();
    }

    bool start()
    {
        const auto exe = modulePath(GetModuleHandleW(nullptr));
        auto command = L"\"" + exe.wstring() + L"\" --plugin \"" + directory_.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process))
            return false;
        CloseHandle(process.hThread);
        handle_ = process.hProcess;
        return true;
    }

    bool finish()
    {
        std::ofstream(directory_ / L"done").put('x');
        DWORD code = 1;
        if (WaitForSingleObject(handle_, 6000) != WAIT_OBJECT_0) {
            // Test owns the child. Keep a stuck fixture from leaking a process.
            TerminateProcess(handle_, 1);
            WaitForSingleObject(handle_, 1000);
        } else {
            GetExitCodeProcess(handle_, &code);
        }
        CloseHandle(handle_);
        handle_ = nullptr;
        return code == 0;
    }

private:
    const fs::path& directory_;
    HANDLE handle_{};
};

int runPluginSide(const fs::path& fixture)
{
    const auto game = installedGameRoot(pluginDirectory(fixture / L"Game A"));
    const auto root = profileDirectory(pluginDirectory(game) / L"ChromeAxis.dll");
    if (game != fixture / L"Game A" || root != pluginDirectory(game) / L"Profil")
        return 10;

    SimulationReceiver simulation(game);
    SimulationReceiver isolated(fixture / L"Game B");
    DiagnosticPublisher diagnostic(game);
    std::uint64_t capturedId{};
    bool invalidSettings = false;
    const auto finished = waitFor([&] {
        std::string error;
        const auto selected = activePreset(root);
        const auto profile = loadProfile(selected.directory / profileFilename, error);
        const auto bindings = loadAccessoryProfile(selected.directory / accessoryFilename, error);
        if (!profile || !bindings || bindings->size() != 1 || profile->steering.deviceId != device ||
            isolated.session() != SimulationSession::inactive) {
            invalidSettings = true;
            return true;
        }

        const bool simulated = simulation.session() != SimulationSession::inactive;
        const bool enabled = usbEnabled(root);
        DiagnosticSnapshot current;
        current.mode = simulated ? DiagnosticMode::simulation : enabled ? DiagnosticMode::usb : DiagnosticMode::usbDisabled;
        if (!selected.name.empty()) {
            const auto length = static_cast<int>(selected.name.size());
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, selected.name.data(),
                length, current.profile.data(), static_cast<int>(current.profile.size() - 1), nullptr, nullptr);
            if (!bytes) {
                invalidSettings = true;
                return true;
            }
        }
        current.profileLoaded = 1;
        current.accessoryCount = static_cast<std::uint32_t>(bindings->size());
        current.axes[0].inputValue = profile->steering.deadzone;
        current.axes[0].inputAvailable = 1;
        current.transmission = {.configured = true, .manual = true, .selectedGear = 3, .requestedGear = 3,
                                .engagedGear = 2, .shift = DiagnosticShiftState::pending,
                                .shortcutKey = loadTransmissionHotkey(root)};
        PerformanceReport report;
        if (const auto request = diagnostic.performanceRequest(); request && request->enabled) {
            report.state = PerformanceState::capturing;
            report.requestId = request->id;
            report.startedAt = GetTickCount64();
            if (capturedId != request->id) {
                diagnostic.recordDrivingSample(request->id, {.sampledAt = GetTickCount64(), .mode = current.mode,
                    .throttle = 0.35f, .throttleAvailable = true, .transmission = current.transmission});
                capturedId = request->id;
            }
        }
        diagnostic.publish(current, 1, report);
        return fs::exists(fixture / L"done");
    }, 25s);
    return finished && !invalidSettings ? 0 : 11;
}

int runApplicationSide()
{
    const auto unique = L"chrome-axis-bridge-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto directory = fs::temp_directory_path() / unique;
    if (!fs::create_directory(directory))
        return 1;
    const Fixture fixture{directory};
    const auto game = directory / L"Game A";
    const auto plugin = pluginDirectory(game);
    fs::create_directories(plugin / L"scripts");
    fs::create_directories(game / L"bin" / L"x64");
    fs::create_directory(directory / L"Game B");
    for (const auto& marker : {plugin / L"ChromeAxis.exe", plugin / L"ChromeAxis.dll",
                               plugin / L"scripts" / L"driving_context.reds",
                               game / L"bin" / L"x64" / L"Cyberpunk2077.exe"})
        std::ofstream(marker).put('x');

    const auto root = calibrationProfileDirectory(plugin / L"ChromeAxis.exe");
    if (root != plugin / L"Profil")
        return 2;
    fs::create_directories(root);
    std::string error;
    Profile profile;
    profile.steering = {device, 0, 0, -1000, 1000, 0.02f};
    const AccessoryProfile bindings{{device, AccessoryKind::Button, 0, 0, 1, VK_SPACE}};
    if (!saveProfile(root / profileFilename, profile, error) ||
        !saveAccessoryProfile(root / accessoryFilename, bindings, error) ||
        !setUsbEnabled(root, true, error) || !saveTransmissionHotkey(root, VK_F9, error) ||
        !createPreset(root, preset, error))
        return 3;
    const auto second = activePreset(root);
    profile.steering.deadzone = 0.13f;
    if (!saveProfile(second.directory / profileFilename, profile, error) || !selectPreset(root, L"", error))
        return 4;

    SimulationPublisher simulation(game);
    if (!simulation.enable())
        return 5;
    DiagnosticReader application(game / L".");
    DiagnosticReader isolated(directory / L"Game B");
    Child child(directory);
    if (!child.start())
        return 6;
    if (!waitFor([&] {
        simulation.heartbeat();
        const auto read = application.read();
        return read.connection == DiagnosticConnection::connected && read.snapshot.mode == DiagnosticMode::simulation &&
            read.snapshot.transmission.shortcutKey == VK_F9 && read.snapshot.transmission.engagedGear == 2 &&
            read.snapshot.axes[0].inputValue == 0.02f;
    }) || isolated.read().connection == DiagnosticConnection::connected)
        return 7;

    std::uint64_t id{};
    std::array<DiagnosticCaptureRecord, 8> rows{};
    bool sawCapture = false;
    bool sawMatchingRow = false;
    if (!waitFor([&] {
        simulation.heartbeat();
        if (!id)
            id = application.requestPerformanceCapture(true);
        if (!id)
            return false;
        sawCapture |= application.read().performance.requestId == id;
        const auto batch = application.drainCapture(rows);
        for (std::size_t i = 0; i < batch.count; ++i) {
            const auto& row = rows[i];
            sawMatchingRow |= !row.usbIncident && row.driving.transmission.requestedGear == 3 &&
                row.driving.throttle == 0.35f;
        }
        return sawCapture && sawMatchingRow;
    }))
        return 8;

    simulation.disable();
    // The fixture polls files much faster than the game. Allow a temporary
    // Windows sharing conflict to clear while changing installation settings.
    if (!waitFor([&] { return selectPreset(root, preset, error); }) ||
        !waitFor([&] { return saveTransmissionHotkey(root, VK_F10, error); })) {
        std::cerr << "Could not switch the fixture profile or hotkey: " << error << '\n';
        return 9;
    }
    if (!waitFor([&] {
        const auto read = application.read();
        return read.connection == DiagnosticConnection::connected && read.snapshot.mode == DiagnosticMode::usb &&
            std::string_view(read.snapshot.profile.data()) == presetUtf8 &&
            read.snapshot.axes[0].inputValue == 0.13f && read.snapshot.transmission.shortcutKey == VK_F10;
    }))
        return 13;
    if (!waitFor([&] { return setUsbEnabled(root, false, error); }) ||
        !waitFor([&] { return application.read().snapshot.mode == DiagnosticMode::usbDisabled; }))
        return 14;
    return child.finish() ? 0 : 12;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--plugin")
        return runPluginSide(argv[2]);
    const auto result = runApplicationSide();
    if (result)
        std::cerr << "Application/plugin bridge failed at stage " << result << '\n';
    return result;
}
