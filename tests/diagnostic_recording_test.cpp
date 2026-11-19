#include "chrome_axis/diagnostics.hpp"
#include "diagnostic_recording.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
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

bool testChronologicalExport(const std::filesystem::path& directory)
{
    constexpr std::uint64_t start = 1000;
    const DiagnosticCaptureRecord first{
        .usb = {.sampledAt = start + 200, .kind = DiagnosticUsbEventKind::pollExpired}, .usbIncident = true};
    const DiagnosticCaptureRecord late{
        .usb = {.sampledAt = start + 195, .durationMicros = 120000,
                .kind = DiagnosticUsbEventKind::deviceSlow}, .usbIncident = true};
    const DiagnosticCaptureRecord laterArrival{
        .usb = {.sampledAt = start + 195, .durationMicros = 200000,
                .kind = DiagnosticUsbEventKind::deviceSlow}, .usbIncident = true};
    const DiagnosticCaptureRecord beforeStart{
        .usb = {.sampledAt = start - 77, .durationMicros = 888877,
                .kind = DiagnosticUsbEventKind::deviceSlow}, .usbIncident = true};
    const auto csv = [&](auto rows) {
        std::ostringstream stream;
        writeDiagnosticCaptureHeader(stream);
        writeDiagnosticCaptureRows(stream, rows, start, 2);
        return std::move(stream).str();
    };
    // The dropped marker precedes the records associated with the missing samples.
    std::ostringstream ordered;
    writeDiagnosticCaptureHeader(ordered);
    writeDiagnosticCaptureRows(ordered, std::array{late, laterArrival}, start, 0);
    writeDiagnosticCaptureRows(ordered, std::array{first}, start, 2);
    const auto source = directory / L"live.csv";
    const auto destination = directory / L"export.csv";
    const auto original = csv(std::array{beforeStart, first, late, laterArrival});
    if (original != csv(std::array{first, late, laterArrival}))
        return false;
    std::ofstream(source, std::ios::binary) << original;
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{input}, {});
    };
    const auto exportState = [](const std::filesystem::path& input, const std::filesystem::path& output) {
        DiagnosticRecorder exporter({}, {});
        if (!exporter.startExport(input, output))
            return DiagnosticExportState::idle;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (exporter.exportState() == DiagnosticExportState::exporting && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        return exporter.exportState();
    };
    const auto sourceAlias = directory / L"." / L"LIVE.CSV";
    const auto hardLink = directory / L"recording-link.csv";
    const auto sortedCopy = directory / L"sorted-copy.csv";
    if (!exportDiagnosticRecording(source, destination) || read(destination) != ordered.str() ||
        !exportDiagnosticRecording(destination, sortedCopy) || read(sortedCopy) != ordered.str() ||
        exportState(source, source) != DiagnosticExportState::sameSource ||
        exportState(source, sourceAlias) != DiagnosticExportState::sameSource ||
        !CreateHardLinkW(hardLink.c_str(), source.c_str(), nullptr) ||
        exportState(source, hardLink) != DiagnosticExportState::sameSource ||
        read(source) != original || read(hardLink) != original)
        return false;

    // A failed background export must preserve the existing destination.
    const auto unordered = directory / L"unordered.csv";
    std::ofstream(unordered, std::ios::binary) << original;
    std::ofstream(destination, std::ios::binary) << "existing export";
    const auto handle = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const std::unique_ptr<void, decltype(&CloseHandle)> locked(handle, CloseHandle);
    if (exportState(unordered, destination) != DiagnosticExportState::failed || read(destination) != "existing export")
        return false;

    const auto sourceBusyExport = directory / L"source-busy-export.csv";
    std::ofstream(sourceBusyExport, std::ios::binary) << "existing export";
    {
        const auto writer = CreateFileW(unordered.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writer == INVALID_HANDLE_VALUE)
            return false;
        const std::unique_ptr<void, decltype(&CloseHandle)> writing(writer, CloseHandle);
        if (exportState(unordered, sourceBusyExport) != DiagnosticExportState::sourceLocked ||
            read(sourceBusyExport) != "existing export")
            return false;
    }
    {
        // An exclusive range can be held even though a second read handle opens successfully.
        const auto reader = CreateFileW(unordered.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reader == INVALID_HANDLE_VALUE)
            return false;
        const std::unique_ptr<void, decltype(&CloseHandle)> reading(reader, CloseHandle);
        OVERLAPPED range{};
        range.Offset = 5;
        if (!LockFileEx(reader, LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK, 0, 4, 0, &range))
            return false;
        const auto result = exportState(unordered, sourceBusyExport);
        if (!UnlockFileEx(reader, 0, 4, 0, &range) || result != DiagnosticExportState::sourceLocked ||
            read(sourceBusyExport) != "existing export")
            return false;
    }
    if (!exportDiagnosticRecording(unordered, sourceBusyExport) || read(sourceBusyExport) != ordered.str())
        return false;

    // Closing the recorder must account for an export that still owns the paths.
    const auto retried = directory / L"retried.csv";
    {
        DiagnosticRecorder exporter({}, {});
        if (!exporter.startExport(unordered, retried))
            return false;
    }
    return read(retried) == ordered.str() && read(unordered) == original;
}

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
    DiagnosticRecorder contender(session.path, profile);
    if (!recorder.start())
        return 1;

    bool emittedIncident = false;
    bool restartedGame = false;
    bool emittedAfterRestart = false;
    constexpr std::string_view wheelId = "{6A523E13-8D9C-4CA3-A19C-8B317DFBE275}";
    constexpr std::string_view pedalId = "{6A523E13-8D9C-4CA3-A19C-8B317DFBE276}";
    const auto pump = [&] {
        if (!publisher) {
            std::this_thread::sleep_for(28ms);
            return;
        }
        if (const auto request = publisher->performanceRequest(); request && request->id) {
            if (request->enabled) {
                if (performance.requestId == request->id && !emittedIncident) {
                    const auto started = recorder.status().startedAt;
                    publisher->recordUsbEvent({.sampledAt = started - 77, .durationMicros = 888877,
                        .kind = DiagnosticUsbEventKind::deviceSlow, .mode = DiagnosticMode::usb});
                    publisher->recordUsbEvent({.sampledAt = GetTickCount64(), .durationMicros = 505520,
                        .kind = DiagnosticUsbEventKind::pollExpired, .mode = DiagnosticMode::usb});
                    DiagnosticUsbEvent returned{.sampledAt = GetTickCount64(), .durationMicros = 510000,
                        .kind = DiagnosticUsbEventKind::deviceSlow, .mode = DiagnosticMode::gameNotForeground,
                        .lane = DiagnosticUsbLane::feedback, .hasCallLane = true, .callStartedAt = started - 50,
                        .callStartMode = DiagnosticMode::usb};
                    constexpr std::string_view call = "FFB SetParameters";
                    std::ranges::copy(call, returned.operation.begin());
                    std::ranges::copy(wheelId, returned.deviceId.begin());
                    publisher->recordUsbEvent(returned);
                    emittedIncident = true;
                }
                if (performance.requestId == request->id && restartedGame && !emittedAfterRestart) {
                    const auto resumed = [&](std::string_view device, std::string_view name) {
                        DiagnosticUsbEvent event{.sampledAt = GetTickCount64(), .kind = DiagnosticUsbEventKind::deviceRecovered,
                            .mode = DiagnosticMode::usb, .lane = DiagnosticUsbLane::reader, .hasCallLane = true};
                        std::ranges::copy(device, event.deviceId.begin());
                        std::ranges::copy(name, event.operation.begin());
                        publisher->recordUsbEvent(event);
                    };
                    resumed(pedalId, "Pedal restart fixture");
                    resumed(wheelId, "Wheel restart fixture");
                    emittedAfterRestart = true;
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

    const auto until = [&](const DiagnosticRecorder& target, auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (predicate(target.status()))
                return true;
        }
        return false;
    };

    // No diagnostic window or UI timer participates in the capture.
    if (!until(recorder, [](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::recording && status.rows >= 3 &&
                !status.file.empty() && std::filesystem::is_regular_file(status.file);
        }))
        return 2;
    const auto first = recorder.status();
    const auto whileRecording = first.file.parent_path() / L"during-recording.csv";
    if (exportDiagnosticRecording(first.file, whileRecording) || std::filesystem::exists(whileRecording))
        return 10;
    if (!contender.start() || !until(contender, [](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::slotOccupied;
        }))
        return 7;
    contender.stop();
    if (!until(contender, [](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::complete && status.rows == 0;
        }) || !until(recorder, [&](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::recording && status.rows > first.rows &&
                status.file == first.file;
        }))
        return 8;
    const auto beforeRestart = recorder.status();
    publisher.reset();
    if (!until(recorder, [](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::waitingForGame;
        }))
        return 3;
    performance = {};
    publisher.emplace(session.path);
    restartedGame = true;
    if (!until(recorder, [&](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::recording && status.rows > beforeRestart.rows &&
                status.file == beforeRestart.file && emittedAfterRestart;
        }))
        return 4;
    recorder.stop();
    if (!until(recorder, [](const DiagnosticRecordingStatus& status) {
            return status.state == DiagnosticRecordingState::complete;
        }))
        return 5;

    const auto result = recorder.status();
    std::ifstream input(result.file, std::ios::binary);
    const std::string csv(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    const auto header = csv.find("elapsed_ms,mode");
    const auto force = csv.find("FFB SetParameters");
    if (force == csv.npos)
        return 6;
    const auto start = csv.rfind('\n', force) + 1;
    const auto forceLine = std::string_view(csv).substr(start, csv.find('\n', force) - start);
    if (!emittedIncident || header == csv.npos || csv.find("elapsed_ms,mode", header + 1) != csv.npos ||
        csv.find("888877") != csv.npos || forceLine.find(",game_not_foreground,") == forceLine.npos ||
        !forceLine.ends_with(",feedback,,-50,usb,1\r") || csv.find("6A523E13") != csv.npos ||
        csv.find("505520") == csv.npos || csv.find(",usb,") == csv.npos ||
        csv.find("connection_lost") == csv.npos || csv.find("connection_restored") == csv.npos || result.dropped)
        return 6;
    const auto hasDeviceSlot = [&](std::string_view name, std::string_view suffix) {
        const auto at = csv.find(name);
        return at != csv.npos && std::string_view(csv).substr(at, csv.find('\n', at) - at).ends_with(suffix);
    };
    if (!hasDeviceSlot("Pedal restart fixture", ",reader,,,,2\r") ||
        !hasDeviceSlot("Wheel restart fixture", ",reader,,,,1\r"))
        return 12;
    input.close();
    const auto afterReconnect = result.file.parent_path() / L"after-reconnect.csv";
    if (!exportDiagnosticRecording(result.file, afterReconnect))
        return 11;
    {
        std::ifstream exported(afterReconnect, std::ios::binary);
        const std::string rows(std::istreambuf_iterator<char>{exported}, std::istreambuf_iterator<char>{});
        if (rows.find("connection_lost") == rows.npos || rows.find("connection_restored") == rows.npos)
            return 11;
    }
    return testChronologicalExport(result.file.parent_path()) ? 0 : 9;
}

} // namespace

int main()
{
    const auto result = run();
    if (result)
        std::cerr << "Continuous recording regression, check " << result << '\n';
    return result;
}
