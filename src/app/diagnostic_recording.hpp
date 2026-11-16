#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace chrome_axis {

enum class DiagnosticRecordingState { idle, starting, recording, waitingForGame, slotOccupied, stopping, complete, failed };
enum class DiagnosticRecordingFailure { none, file, unavailable, stopTimeout, worker };

struct DiagnosticRecordingStatus {
    DiagnosticRecordingState state{DiagnosticRecordingState::idle};
    std::filesystem::path file;
    std::uint64_t startedAt{};
    std::uint64_t rows{};
    std::uint64_t dropped{};
    DiagnosticRecordingFailure failure{DiagnosticRecordingFailure::none};
};

bool exportDiagnosticRecording(const std::filesystem::path& source, const std::filesystem::path& destination);

class DiagnosticRecorder {
public:
    DiagnosticRecorder(const std::filesystem::path& gameRoot, const std::filesystem::path& profileRoot);
    ~DiagnosticRecorder();
    DiagnosticRecorder(const DiagnosticRecorder&) = delete;
    DiagnosticRecorder& operator=(const DiagnosticRecorder&) = delete;

    bool start();
    void stop() noexcept;
    DiagnosticRecordingStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chrome_axis
