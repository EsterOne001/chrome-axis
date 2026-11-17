#pragma once

#include "chrome_axis/input.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace chrome_axis {

struct DeviceLayoutRequest {
    std::string id;
    DeviceObjectSelection wanted;
    std::uint64_t connection{};
};

struct DeviceLayoutSnapshot {
    DeviceObjectSelection present;
    DeviceObjectSelection pending;
    DeviceObjectSelection fallback; // Objects that can be checked on the acquired reader.
    bool probeOverdue{};

    bool incomplete() const noexcept {
        return pending.axes.any() || pending.buttons.any() || pending.povs.any();
    }

    void changeSelection(const DeviceObjectSelection& previous, const DeviceObjectSelection& wanted) noexcept {
        const auto retain = [](auto& present, auto& pending, const auto& previous, const auto& wanted) {
            present &= previous & wanted;
            pending = (pending | ~previous) & wanted;
        };
        retain(present.axes, pending.axes, previous.axes, wanted.axes);
        retain(present.buttons, pending.buttons, previous.buttons, wanted.buttons);
        retain(present.povs, pending.povs, previous.povs, wanted.povs);
        fallback.axes &= previous.axes & wanted.axes;
        fallback.buttons &= previous.buttons & wanted.buttons;
        fallback.povs &= previous.povs & wanted.povs;
    }
};

// Query is called only on the discovery thread. Sampling never waits on the driver.
template<class Query>
class DeviceLayoutWorker {
public:
    explicit DeviceLayoutWorker(Query query, const DirectInputActiveCall* activeCall = nullptr)
        : activeCall_(activeCall), thread_([this, query = std::move(query)](std::stop_token stop) mutable {
            try {
                run(query, stop);
            } catch (...) {
                // Keep the last published layout if discovery cannot continue.
            }
        }) {}

    DeviceLayoutSnapshot read(const std::string& id, const DeviceObjectSelection& wanted, std::uint64_t connection) {
        const auto now = DeviceReadClock::now();
        std::scoped_lock lock(mutex_);
        const bool sameConnection = request_.connection == connection && request_.id == id;
        if (!sameConnection || request_.wanted != wanted) {
            const auto previous = request_.wanted;
            request_ = {id, wanted, connection};
            if (sameConnection)
                snapshot_.changeSelection(previous, wanted);
            else
                snapshot_ = {.pending = wanted};
            retryAt_ = {};
            ++revision_;
            changed_.notify_one();
        } else if (now - lastDemand_ > demandLifetime) {
            changed_.notify_one();
        }
        lastDemand_ = now;
        auto snapshot = snapshot_;
        if (activeCall_ && snapshot.incomplete() && probingDevice_ == id) {
            const auto call = activeCall_->snapshot();
            snapshot.probeOverdue = call.overdue(GetTickCount64(), probeTimeoutMilliseconds);
        }
        return snapshot;
    }

private:
    using Clock = DeviceReadClock;
    static constexpr auto demandLifetime = std::chrono::milliseconds(250);
    static constexpr std::uint64_t probeTimeoutMilliseconds = 1000;

    void run(Query& query, std::stop_token stop) {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            changed_.wait(lock, stop, [&] {
                return snapshot_.incomplete() && Clock::now() - lastDemand_ <= demandLifetime;
            });
            if (stop.stop_requested())
                break;
            const auto revision = revision_;
            if (Clock::now() < retryAt_) {
                changed_.wait_until(lock, stop, retryAt_, [&] { return revision_ != revision; });
                continue;
            }
            const auto request = request_;
            auto result = snapshot_;
            probingDevice_ = request.id;
            lock.unlock();
            try {
                result = query(request);
            } catch (...) {
                // A failed discovery leaves the affected objects pending for retry.
            }
            lock.lock();
            probingDevice_ = {};
            if (revision_ == revision) {
                snapshot_ = result;
                retryAt_ = Clock::now() + std::chrono::milliseconds(500);
            }
        }
    }

    const DirectInputActiveCall* activeCall_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    DeviceLayoutRequest request_;
    std::string_view probingDevice_; // Backed by the query's request while it is running.
    DeviceLayoutSnapshot snapshot_;
    Clock::time_point lastDemand_;
    Clock::time_point retryAt_;
    std::uint64_t revision_{};
    std::jthread thread_;
};

} // namespace chrome_axis
