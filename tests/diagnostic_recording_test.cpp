#include "chrome_axis/diagnostics.hpp"
#include "diagnostic_recording.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace chrome_axis;

struct SessionDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"chrome-axis-recording-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    ~SessionDirectory()
    {
        std::error_code error;
        const auto recordings = path / L"Profil" / L"Diagnostics";
        for (std::filesystem::directory_iterator file(recordings, error), end; !error && file != end; file.increment(error))
            std::filesystem::remove(file->path(), error);
        std::filesystem::remove(recordings, error);
        std::filesystem::remove(recordings.parent_path(), error);
        std::filesystem::remove(path, error);
    }
};

int run()
{
    const SessionDirectory session;
    const auto profile = session.path / L"Profil";
    std::filesystem::create_directories(profile);

    std::optional<DiagnosticPublisher> publisher(std::in_place, session.path);
    const DiagnosticSnapshot snapshot{.mode = DiagnosticMode::usb};
    PerformanceReport performance;
    publisher->publish(snapshot);
    DiagnosticRecorder recorder(session.path, profile);
    if (!recorder.start())
        return 1;

    bool emittedIncident = false;
    const auto pump = [&] {
        if (!publisher) {
            std::this_thread::sleep_for(28ms);
            return;
        }
        if (const auto request = publisher->performanceRequest(); request && request->id) {
            if (request->enabled) {
                if (performance.requestId == request->id && !emittedIncident) {
                    publisher->recordUsbEvent({.sampledAt = GetTickCount64(), .durationMicros = 505520,
                        .kind = DiagnosticUsbEventKind::pollExpired, .mode = DiagnosticMode::usb});
                    emittedIncident = true;
                }
                publisher->recordDrivingSample(request->id, {.sampledAt = GetTickCount64(), .mode = DiagnosticMode::usb,
                    .speedMps = 13.0f, .speedAvailable = true});
                performance = {.state = PerformanceState::capturing, .requestId = request->id};
            } else {
                performance = {.state = PerformanceState::complete, .requestId = request->id};
            }
        }
        publisher->publish(snapshot, 1, performance);
        std::this_thread::sleep_for(28ms);
    };

    const auto until = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (predicate(recorder.status()))
                return true;
        }
        return false;
    };

    // No diagnostic window or UI timer participates in the capture.
    if (!until([](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::recording && status.rows >= 3 &&
                !status.file.empty() && std::filesystem::is_regular_file(status.file);
        }))
        return 2;
    const auto beforeRestart = recorder.status();
    publisher.reset();
    if (!until([](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::waitingForGame;
        }))
        return 3;
    performance = {};
    publisher.emplace(session.path);
    if (!until([&](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::recording && status.rows > beforeRestart.rows &&
                status.file == beforeRestart.file;
        }))
        return 4;
    recorder.stop();
    if (!until([](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::complete;
        }))
        return 5;

    const auto result = recorder.status();
    std::ifstream input(result.file, std::ios::binary);
    const std::string csv(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    const auto header = csv.find("elapsed_ms,mode");
    if (!emittedIncident || header == csv.npos || csv.find("elapsed_ms,mode", header + 1) != csv.npos ||
        csv.find("505520") == csv.npos || csv.find(",usb,") == csv.npos ||
        csv.find("connection_lost") == csv.npos || csv.find("connection_restored") == csv.npos || result.dropped)
        return 6;
    return 0;
}

} // namespace

int main()
{
    const auto result = run();
    if (result)
        std::cerr << "Continuous recording regression, check " << result << '\n';
    return result;
}
