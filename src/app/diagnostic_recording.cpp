#include "diagnostic_recording.hpp"

#include "chrome_axis/diagnostics.hpp"
#include "../core/settings_replace.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <compare>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace chrome_axis {
namespace {

using namespace std::chrono_literals;

constexpr auto pollInterval = 100ms;
constexpr auto flushInterval = 1s;
constexpr auto stopProgressTimeout = 3s;
constexpr std::size_t batchSize = 256;
constexpr std::size_t exportBufferSize = 64 * 1024;

bool active(DiagnosticRecordingState state) noexcept
{
    return state == DiagnosticRecordingState::starting || state == DiagnosticRecordingState::recording ||
        state == DiagnosticRecordingState::waitingForGame || state == DiagnosticRecordingState::slotOccupied ||
        state == DiagnosticRecordingState::stopping;
}

class CaptureFile {
public:
    CaptureFile() = default;
    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;

    ~CaptureFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            flush();
            CloseHandle(handle_);
        }
    }

    bool open(const std::filesystem::path& directory, std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return false;

        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto stem = std::format(L"ChromeAxis-Diagnostic-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentProcessId());
        for (std::uint64_t suffix = 0;; ++suffix) {
            auto candidate = directory / std::format(L"{}-{}.csv", stem, suffix);
            if (create(candidate)) {
                path = std::move(candidate);
                return true;
            }
            const auto lastError = GetLastError();
            if (lastError != ERROR_FILE_EXISTS && lastError != ERROR_ALREADY_EXISTS)
                return false;
        }
    }

    bool create(const std::filesystem::path& path) noexcept
    {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool append(std::string_view text) noexcept
    {
        if (text.empty())
            return true;
        // The caller formats at most one bounded batch at a time.
        const auto size = static_cast<DWORD>(text.size());
        DWORD written{};
        dirty_ = true;
        if (WriteFile(handle_, text.data(), size, &written, nullptr) && written == size) {
            length_ += written;
            return true;
        }
        LARGE_INTEGER previousEnd{};
        previousEnd.QuadPart = length_;
        if (SetFilePointerEx(handle_, previousEnd, nullptr, FILE_BEGIN))
            SetEndOfFile(handle_);
        return false;
    }

    bool flush() noexcept
    {
        if (dirty_ && !FlushFileBuffers(handle_))
            return false;
        dirty_ = false;
        return true;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    LONGLONG length_{};
    bool dirty_{};
};

class CaptureSession {
public:
    CaptureSession(const std::filesystem::path& gameRoot, DiagnosticRecordingStatus& status)
        : reader(gameRoot), status_(status)
    {
    }

    bool open(const std::filesystem::path& directory)
    {
        if (!file_.open(directory, status_.file))
            return false;
        std::ostringstream output;
        writeDiagnosticCaptureHeader(output);
        nextFlush_ = std::chrono::steady_clock::now() + flushInterval;
        return file_.append(output.view()) && file_.flush();
    }

    bool appendConnection(bool connected)
    {
        std::ostringstream output;
        writeDiagnosticCaptureConnection(output, status_.startedAt, GetTickCount64(), connected);
        return file_.append(output.view());
    }

    bool drain(DiagnosticCaptureBatch& batch)
    {
        batch = reader.drainCapture(records_);
        if (!batch.count && !batch.dropped)
            return true;

        std::ostringstream output;
        writeDiagnosticCaptureRows(output, {records_.data(), batch.count}, status_.startedAt, batch.dropped);
        if (!file_.append(output.view()))
            return false;
        status_.rows += batch.count;
        status_.dropped += batch.dropped;
        return true;
    }

    bool flush() noexcept { return file_.flush(); }

    bool flushIfDue() noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextFlush_)
            return true;
        nextFlush_ = now + flushInterval;
        return flush();
    }

    DiagnosticReader reader;

private:
    DiagnosticRecordingStatus& status_;
    CaptureFile file_;
    std::array<DiagnosticCaptureRecord, batchSize> records_{};
    std::chrono::steady_clock::time_point nextFlush_{};
};

// A recording is written as samples arrive. Only delayed rows need an index
// for chronological export, so ordinary driving rows can be streamed.
struct CsvRowPosition {
    std::uint64_t elapsedMs{};
    std::streamoff offset{};

    auto operator<=>(const CsvRowPosition&) const = default;
};

std::uint64_t csvElapsed(std::string_view row, std::uint64_t previous) noexcept
{
    const auto field = row.substr(0, row.find(','));
    std::uint64_t elapsed{};
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, elapsed);
    return parsed.ec == std::errc{} && parsed.ptr == end ? elapsed : previous;
}

bool writeChronologicalRecording(const std::filesystem::path& source, const std::filesystem::path& staged,
                                 bool& created)
{
    std::ifstream input(source, std::ios::binary);
    std::string header;
    if (!std::getline(input, header))
        return false;
    const auto firstRow = input.tellg();
    std::vector<CsvRowPosition> delayed;
    std::string row;
    std::uint64_t latest{};
    while (input) {
        const auto offset = input.tellg();
        if (!std::getline(input, row))
            break;
        const auto elapsed = csvElapsed(row, latest);
        if (elapsed < latest)
            delayed.push_back({elapsed, offset});
        else
            latest = elapsed;
    }
    if (!input.eof() || input.bad())
        return false;

    if (delayed.empty()) {
        input.close();
        created = CopyFileW(source.c_str(), staged.c_str(), TRUE) != FALSE;
        if (!created) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                DeleteFileW(staged.c_str());
        }
        return created;
    }

    std::ranges::sort(delayed);
    input.clear();
    input.seekg(firstRow);
    std::ifstream delayedInput(source, std::ios::binary);
    CaptureFile output;
    if (!input || !delayedInput || !output.create(staged))
        return false;
    created = true;
    std::string buffer = std::move(header);
    buffer.push_back('\n');
    const auto append = [&](std::string_view line) {
        buffer.append(line);
        buffer.push_back('\n');
        if (buffer.size() < exportBufferSize)
            return true;
        const auto written = output.append(buffer);
        buffer.clear();
        return written;
    };
    auto next = delayed.begin();
    std::string delayedRow;
    const auto appendDelayed = [&] {
        delayedInput.clear();
        delayedInput.seekg(next->offset);
        ++next;
        return std::getline(delayedInput, delayedRow) && append(delayedRow);
    };

    latest = 0;
    while (input) {
        const auto offset = input.tellg();
        if (!std::getline(input, row))
            break;
        const auto elapsed = csvElapsed(row, latest);
        if (elapsed < latest)
            continue;
        latest = elapsed;
        while (next != delayed.end() && *next < CsvRowPosition{elapsed, offset}) {
            if (!appendDelayed())
                return false;
        }
        if (!append(row))
            return false;
    }
    if (!input.eof() || input.bad())
        return false;
    while (next != delayed.end()) {
        if (!appendDelayed())
            return false;
    }
    return output.append(buffer) && output.flush();
}

} // namespace

bool exportDiagnosticRecording(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    const auto staged = detail::stagedSettingsPath(destination);
    bool created = false;
    try {
        if (writeChronologicalRecording(source, staged, created) && detail::replaceSettingsFile(staged, destination))
            return true;
    } catch (const std::exception&) {}
    if (created)
        DeleteFileW(staged.c_str());
    return false;
}

struct DiagnosticRecorder::Impl {
    Impl(const std::filesystem::path& gameRoot, const std::filesystem::path& profileRoot)
        : gameRoot_(gameRoot), directory_(profileRoot.empty() ? std::filesystem::path{} : profileRoot / L"Diagnostics")
    {
    }

    bool start()
    {
        std::lock_guard lock(mutex_);
        if (active(status_.state))
            return false;

        auto previous = status_;
        status_ = {.state = DiagnosticRecordingState::starting, .startedAt = GetTickCount64()};
        try {
            worker_ = std::jthread([this, startedAt = status_.startedAt, previous](std::stop_token stop) mutable {
                run(stop, startedAt, std::move(previous));
            });
        } catch (const std::system_error&) {
            status_ = previous;
            status_.state = DiagnosticRecordingState::failed;
            status_.failure = DiagnosticRecordingFailure::worker;
            return false;
        }
        return true;
    }

    void stop() noexcept
    {
        std::lock_guard lock(mutex_);
        if (active(status_.state)) {
            status_.state = DiagnosticRecordingState::stopping;
            worker_.request_stop();
        }
    }

    DiagnosticRecordingStatus status() const
    {
        std::lock_guard lock(mutex_);
        return status_;
    }

private:
    void reportProgress(const DiagnosticRecordingStatus& current, DiagnosticRecordingState state)
    {
        std::lock_guard lock(mutex_);
        if (status_.state != DiagnosticRecordingState::stopping)
            status_.state = state;
        if (status_.file.empty())
            status_.file = current.file;
        status_.rows = current.rows;
        status_.dropped = current.dropped;
    }

    DiagnosticRecordingFailure endCapture(CaptureSession& session, const DiagnosticRecordingStatus& current)
    {
        auto deadline = std::chrono::steady_clock::now() + stopProgressTimeout;
        bool stopSent = false;
        for (;;) {
            session.reader.read();
            if (!stopSent && session.reader.ownsPerformanceCapture()) {
                stopSent = session.reader.requestPerformanceCapture(false) != 0;
                if (stopSent)
                    deadline = std::chrono::steady_clock::now() + stopProgressTimeout;
            }

            DiagnosticCaptureBatch batch;
            if (!session.drain(batch) || !session.flushIfDue())
                return DiagnosticRecordingFailure::file;
            if (batch.finished)
                break;
            if (batch.count || batch.dropped) {
                deadline = std::chrono::steady_clock::now() + stopProgressTimeout;
                reportProgress(current, DiagnosticRecordingState::stopping);
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return DiagnosticRecordingFailure::stopTimeout;
            if (batch.count < batchSize)
                std::this_thread::sleep_for(pollInterval);
        }
        return DiagnosticRecordingFailure::none;
    }

    DiagnosticRecordingFailure capture(std::stop_token stop, DiagnosticRecordingStatus& current)
    {
        if (gameRoot_.empty())
            return DiagnosticRecordingFailure::unavailable;
        if (directory_.empty())
            return DiagnosticRecordingFailure::file;

        CaptureSession session(gameRoot_, current);
        if (!session.open(directory_))
            return DiagnosticRecordingFailure::file;

        std::uint64_t captureId{};
        bool wasConnected = false;
        bool hadCapture = false;
        while (!stop.stop_requested()) {
            const auto read = session.reader.read();
            if (read.connection == DiagnosticConnection::unavailable)
                return DiagnosticRecordingFailure::unavailable;

            DiagnosticCaptureBatch batch;
            if (captureId && !session.drain(batch))
                return DiagnosticRecordingFailure::file;
            if (batch.finished)
                captureId = 0;

            const bool connected = read.connection == DiagnosticConnection::connected;
            const bool drainedBacklog = batch.finished || batch.count < batchSize;
            if (wasConnected && (batch.finished || (!connected && drainedBacklog))) {
                if (!session.appendConnection(false))
                    return DiagnosticRecordingFailure::file;
                wasConnected = false;
            }
            if (!captureId && connected && !stop.stop_requested())
                captureId = session.reader.requestPerformanceCapture(true);
            if (connected && captureId && !wasConnected) {
                if (hadCapture && !session.appendConnection(true))
                    return DiagnosticRecordingFailure::file;
                wasConnected = true;
                hadCapture = true;
            }

            if (!session.flushIfDue())
                return DiagnosticRecordingFailure::file;
            auto state = DiagnosticRecordingState::waitingForGame;
            if (connected) {
                state = DiagnosticRecordingState::starting;
                if (captureId)
                    state = DiagnosticRecordingState::recording;
                else if (session.reader.captureSlotOccupied())
                    state = DiagnosticRecordingState::slotOccupied;
            }
            reportProgress(current, state);
            if (batch.count < batchSize && !stop.stop_requested())
                std::this_thread::sleep_for(pollInterval);
        }

        const auto failure = captureId ? endCapture(session, current) : DiagnosticRecordingFailure::none;
        return session.flush() ? failure : DiagnosticRecordingFailure::file;
    }

    void run(std::stop_token stop, std::uint64_t startedAt, DiagnosticRecordingStatus previous)
    {
        DiagnosticRecordingStatus current{.state = DiagnosticRecordingState::starting, .startedAt = startedAt};
        try {
            current.failure = capture(stop, current);
        } catch (...) {
            current.failure = DiagnosticRecordingFailure::worker;
        }
        if (current.file.empty() && !previous.file.empty()) {
            const auto failure = current.failure;
            current = std::move(previous);
            current.failure = failure;
        }
        current.state = current.failure == DiagnosticRecordingFailure::none ?
            DiagnosticRecordingState::complete : DiagnosticRecordingState::failed;

        std::lock_guard lock(mutex_);
        status_ = std::move(current);
    }

    std::filesystem::path gameRoot_;
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    DiagnosticRecordingStatus status_;
    // Keeping the worker last joins it before destroying the state and paths it uses.
    std::jthread worker_;
};

DiagnosticRecorder::DiagnosticRecorder(const std::filesystem::path& gameRoot, const std::filesystem::path& profileRoot)
    : impl_(std::make_unique<Impl>(gameRoot, profileRoot))
{
}

DiagnosticRecorder::~DiagnosticRecorder() = default;

bool DiagnosticRecorder::start()
{
    return impl_->start();
}

void DiagnosticRecorder::stop() noexcept
{
    impl_->stop();
}

DiagnosticRecordingStatus DiagnosticRecorder::status() const
{
    return impl_->status();
}

} // namespace chrome_axis
