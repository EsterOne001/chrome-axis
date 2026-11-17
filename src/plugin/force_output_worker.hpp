#pragma once

#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace chrome_axis {

// Only this worker owns output-side DirectInput objects. The polling thread
// publishes at most one pending value and never waits for a driver call.
template <class Context, class Driver = DirectInput, class WorkerThread = std::jthread>
class ForceOutputWorker {
    using Clock = std::chrono::steady_clock;
    using DeviceId = std::array<char, 39>;
    static constexpr auto retryDelay = std::chrono::seconds(2);

public:
    struct Completion {
        std::uint64_t generation{};
        std::uint64_t captureId{};
        std::uint64_t durationMicros{};
        std::uint64_t submittedAt{};
        WheelForceState state{WheelForceState::waiting};
        std::optional<float> accepted;
        bool missedCadence{};
    };

    struct Operation {
        DeviceId deviceId{};
        std::array<char, 40> name{};
        HRESULT result{};
        std::uint64_t durationMicros{};
        std::uint64_t sampledAt{};
        DiagnosticMode mode{DiagnosticMode::usb};
    };

    struct ActiveOperation {
        DeviceId deviceId{};
        DirectInputActiveCall::Snapshot call;
    };

    using ObserveMode = DiagnosticMode (*)() noexcept;
    explicit ForceOutputWorker(DirectInputDiagnostic logger = nullptr, ObserveMode observeMode = nullptr) noexcept
        : logger_(logger), observeMode_(observeMode) {}

    bool submit(HWND window, std::string_view id, std::size_t axis, float force, Context validity,
                std::uint64_t captureId = 0, bool missedCadence = false, std::uint64_t recoveryToken = 0) {
        if (id.size() != DeviceId{}.size() - 1 || !validity.current())
            return false;
        std::unique_lock lock(mutex_);
        if (stopping_ || Clock::now() < nextRequestAt_)
            return false;
        if (!worker_.joinable()) {
            try {
                worker_ = WorkerThread([this](std::stop_token stop) { runRecovering(stop); });
            } catch (const std::system_error&) {
                nextRequestAt_ = Clock::now() + retryDelay;
                return false;
            }
        }
        Request request{window, {}, axis, force, std::move(validity), generation(), captureId, recoveryToken, missedCadence};
        std::ranges::copy(id, request.id.begin());
        pending_ = std::move(request);
        enabled_ = true;
        lock.unlock();
        changed_.notify_one();
        return true;
    }

    void cancel() {
        {
            std::scoped_lock lock(mutex_);
            if (!enabled_)
                return;
            invalidateRequests();
        }
        changed_.notify_one();
    }

    void requestStop() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            if (enabled_)
                invalidateRequests();
        }
        worker_.request_stop();
    }

    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    std::optional<Completion> nextCompletion() {
        std::scoped_lock lock(mutex_);
        return completions_.pop();
    }

    std::optional<Operation> nextOperation() {
        std::scoped_lock lock(mutex_);
        return operations_.pop();
    }

    ActiveOperation activeOperation() {
        std::scoped_lock lock(mutex_);
        return {activeDevice_, activeCall_.snapshot()};
    }

private:
    template <class Record, std::size_t Capacity>
    class Recent {
    public:
        void push(Record record) noexcept {
            if (count_ == Capacity) {
                first_ = (first_ + 1) % Capacity;
                --count_;
            }
            records_[(first_ + count_++) % Capacity] = std::move(record);
        }

        std::optional<Record> pop() noexcept {
            if (!count_)
                return std::nullopt;
            auto result = records_[first_];
            first_ = (first_ + 1) % Capacity;
            --count_;
            return result;
        }

    private:
        std::array<Record, Capacity> records_{};
        std::size_t first_{};
        std::size_t count_{};
    };

    struct Request {
        HWND window{};
        DeviceId id{};
        std::size_t axis{};
        float force{};
        Context context;
        std::uint64_t generation{};
        std::uint64_t captureId{};
        std::uint64_t recoveryToken{};
        bool missedCadence{};

        bool sameTarget(const Request& other) const noexcept {
            return generation == other.generation && window == other.window && id == other.id &&
                axis == other.axis && recoveryToken == other.recoveryToken;
        }
    };

    struct Guard {
        ForceOutputWorker& owner;
        const Request& request;
        std::stop_token stop;

        static bool current(const void* opaque) noexcept {
            const auto& guard = *static_cast<const Guard*>(opaque);
            return guard.check(false);
        }

        static bool canInitialize(const void* opaque) noexcept {
            const auto& guard = *static_cast<const Guard*>(opaque);
            return guard.check(true);
        }

        bool check(bool allowNewer) const noexcept {
            std::scoped_lock lock(owner.mutex_);
            if (stop.stop_requested() || owner.generation() != request.generation)
                return false;
            const auto& pending = owner.pending_;
            if (pending && !request.sameTarget(*pending))
                return false;
            return request.context.current() || (allowNewer && pending && pending->context.current());
        }
    };

    static void observe(void* opaque, const DirectInputReadEvent& event) noexcept {
        if (event.kind != DirectInputReadEventKind::slow)
            return;
        auto& worker = *static_cast<ForceOutputWorker*>(opaque);
        Operation operation{.result = event.result, .durationMicros = event.durationMicros,
                            .sampledAt = GetTickCount64(),
                            .mode = worker.observeMode_ ? worker.observeMode_() : DiagnosticMode::usb};
        std::ranges::copy(event.deviceId.substr(0, operation.deviceId.size() - 1), operation.deviceId.begin());
        std::ranges::copy(event.operation.substr(0, operation.name.size() - 1), operation.name.begin());
        std::scoped_lock lock(worker.mutex_);
        worker.operations_.push(operation);
    }

    void invalidateRequests() noexcept {
        enabled_ = false;
        pending_.reset();
        generation_.fetch_add(1, std::memory_order_release);
    }

    void runRecovering(std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                run(stop);
            } catch (...) {
                // The failed driver is gone. Discard every queued command from
                // its generation and wait for a fresh command after the delay.
                std::scoped_lock lock(mutex_);
                invalidateRequests();
                nextRequestAt_ = Clock::now() + retryDelay;
            }
        }
    }

    void run(std::stop_token stop) {
        std::optional<Driver> driver;
        HWND window{};
        DeviceId deviceId{};
        std::uint64_t observedGeneration = generation();
        auto deadline = Clock::time_point::max();
        Clock::time_point retryAt{};
        std::uint64_t recoveryToken{};

        while (!stop.stop_requested()) {
            std::optional<Request> request;
            std::uint64_t currentGeneration{};
            {
                std::unique_lock lock(mutex_);
                const auto hasWork = [&] { return pending_.has_value() || generation() != observedGeneration; };
                if (deadline == Clock::time_point::max())
                    changed_.wait(lock, stop, hasWork);
                else
                    changed_.wait_until(lock, stop, deadline, hasWork);
                if (stop.stop_requested())
                    break;
                currentGeneration = generation();
                request = std::exchange(pending_, std::nullopt);
            }
            if (observedGeneration != currentGeneration ||
                (Clock::now() >= deadline && (!request || !request->context.current()))) {
                if (driver)
                    driver->stopWheelForce();
                observedGeneration = currentGeneration;
                deadline = Clock::time_point::max();
            }
            if (!request)
                continue;
            const Guard guard{*this, *request, stop};
            if (!Guard::current(&guard))
                continue;

            const auto started = Clock::now();
            deadline = request->context.deadline();
            if (request->window != window) {
                driver.reset();
                window = request->window;
                retryAt = {};
            }
            if (request->recoveryToken && request->recoveryToken != recoveryToken) {
                recoveryToken = request->recoveryToken;
                retryAt = {};
            }
            if (request->id != deviceId) {
                deviceId = request->id;
                std::scoped_lock lock(mutex_);
                activeDevice_ = deviceId;
            }
            if (!driver && started >= retryAt) {
                driver.emplace(window, logger_, &observe, this, &activeCall_);
                if (!driver->ready()) {
                    driver.reset();
                    retryAt = Clock::now() + retryDelay;
                }
            }
            std::optional<float> accepted;
            if (driver && Guard::current(&guard))
                accepted = driver->setWheelForce({request->id.data(), request->id.size() - 1},
                    request->axis, request->force,
                    {&Guard::current, &Guard::canInitialize, &guard, request->recoveryToken});
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
            const Completion result{
                .generation = request->generation, .captureId = request->captureId,
                .durationMicros = static_cast<std::uint64_t>(duration.count()),
                .submittedAt = accepted ? GetTickCount64() : 0,
                .state = driver ? driver->wheelForceState() : WheelForceState::unavailable,
                .accepted = accepted,
                .missedCadence = request->missedCadence,
            };
            std::scoped_lock lock(mutex_);
            completions_.push(result);
        }
        if (driver)
            driver->stopWheelForce();
    }

    DirectInputDiagnostic logger_{};
    ObserveMode observeMode_{};
    std::atomic<std::uint64_t> generation_{};
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<Request> pending_;
    Recent<Completion, 8> completions_;
    Recent<Operation, 16> operations_;
    DirectInputActiveCall activeCall_;
    DeviceId activeDevice_{};
    Clock::time_point nextRequestAt_{};
    bool enabled_{};
    bool stopping_{};
    WorkerThread worker_;
};

} // namespace chrome_axis
