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

    bool incomplete() const noexcept {
        return pending.axes.any() || pending.buttons.any() || pending.povs.any();
    }
};

// Query is called only on the discovery thread. Sampling never waits on the driver.
template<class Query>
class DeviceLayoutWorker {
public:
    explicit DeviceLayoutWorker(Query query)
        : thread_([this, query = std::move(query)](std::stop_token stop) mutable {
            try {
                run(query, stop);
            } catch (...) {
                // Keep the last published layout if discovery cannot continue.
            }
        }) {}

    DeviceLayoutSnapshot read(const std::string& id, const DeviceObjectSelection& wanted, std::uint64_t connection) {
        const auto now = DeviceReadClock::now();
        std::scoped_lock lock(mutex_);
        if (request_.connection != connection || request_.wanted != wanted || request_.id != id) {
            request_ = {id, wanted, connection};
            snapshot_ = {.pending = wanted};
            retryAt_ = {};
            ++revision_;
            changed_.notify_one();
        } else if (now - lastDemand_ > demandLifetime) {
            changed_.notify_one();
        }
        lastDemand_ = now;
        return snapshot_;
    }

private:
    using Clock = DeviceReadClock;
    static constexpr auto demandLifetime = std::chrono::milliseconds(250);

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
            lock.unlock();
            auto result = DeviceLayoutSnapshot{.pending = request.wanted};
            try {
                result = query(request);
            } catch (...) {
                // A failed discovery leaves the affected objects pending for retry.
            }
            lock.lock();
            if (revision_ == revision) {
                snapshot_ = result;
                retryAt_ = Clock::now() + std::chrono::milliseconds(500);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable_any changed_;
    DeviceLayoutRequest request_;
    DeviceLayoutSnapshot snapshot_;
    Clock::time_point lastDemand_;
    Clock::time_point retryAt_;
    std::uint64_t revision_{};
    std::jthread thread_;
};

} // namespace chrome_axis
