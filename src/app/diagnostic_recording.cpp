#include "diagnostic_recording.hpp"

#include "chrome_axis/diagnostics.hpp"

#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace chrome_axis {
namespace {

using namespace std::chrono_literals;

constexpr auto pollInterval = 100ms;
constexpr auto flushInterval = 1s;
constexpr auto stopProgressTimeout = 3s;
constexpr std::size_t batchSize = 256;

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
            handle_ = CreateFileW(candidate.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                path = std::move(candidate);
                return true;
            }
            const auto lastError = GetLastError();
            if (lastError != ERROR_FILE_EXISTS && lastError != ERROR_ALREADY_EXISTS)
                return false;
        }
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

} // namespace

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
